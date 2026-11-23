#include "schematic/JavaSchematic.h"
#include "schematic/JavaNbt.h"
#include "storage/Nbt.h"
#include "mc/world/level/block/Block.h"
#include <bit>
#include <chrono>
#include <limits>
#include <utility>

namespace creatorx::schematic {
namespace {
using core::Vec3i;
using storage::Nbt;
constexpr std::string_view air = "minecraft:air";
constexpr std::string_view transparent = "minecraft:structure_void";

int coordinate(std::int64_t value) {
    if (!std::in_range<int>(value)) throw std::runtime_error("structure coordinates exceed integer range");
    return int(value);
}

Vec3i sum(Vec3i a, Vec3i b) {
    return {coordinate(std::int64_t(a.x) + b.x), coordinate(std::int64_t(a.y) + b.y), coordinate(std::int64_t(a.z) + b.z)};
}

Vec3i dimensions(core::Box const& box) {
    auto size = [](int low, int high) { return coordinate(std::int64_t(high) - low + 1); };
    return {size(box.min.x, box.max.x), size(box.min.y, box.max.y), size(box.min.z, box.max.z)};
}

std::uint64_t volume(Vec3i size, std::uint64_t limit) {
    if (size.x <= 0 || size.y <= 0 || size.z <= 0) throw std::runtime_error("invalid Java structure dimensions");
    core::CuboidSize extent{std::uint64_t(size.x), std::uint64_t(size.y), std::uint64_t(size.z)};
    if (!extent.fits(limit)) throw std::runtime_error("Java structure size exceeds selection limit");
    return extent.volume();
}

Vec3i vector(CompoundTag const& tag) { return {int(tag["x"]), int(tag["y"]), int(tag["z"])}; }
CompoundTag vector(Vec3i v) { return {{"x", v.x}, {"y", v.y}, {"z", v.z}}; }

Vec3i intArray(Nbt const& value) {
    auto const& list = value.get<IntArrayTag>();
    if (list.size() != 3) throw std::runtime_error("invalid Java structure offset");
    return {list[0], list[1], list[2]};
}

size_t listSize(CompoundTag const& tag, std::string_view name) {
    return tag.contains(name) ? tag[name].get<ListTag>().size() : 0;
}

std::string stateName(CompoundTag const& tag) {
    std::string name{std::string_view(tag["Name"])};
    if (!tag.contains("Properties")) return name;
    auto const& properties = tag["Properties"].get<CompoundTag>();
    bool first = true;
    for (auto const& [key, value] : properties.mTags) {
        name += first ? '[' : ',';
        name += key;
        name += '=';
        name += std::string_view(value);
        first = false;
    }
    if (!first) name += ']';
    return name;
}

CompoundTag stateTag(std::string_view name) {
    auto bracket = name.find('[');
    CompoundTag tag{{"Name", name.substr(0, bracket)}};
    if (bracket == std::string_view::npos) return tag;
    CompoundTag properties;
    auto values = name.substr(bracket + 1, name.size() - bracket - 2);
    while (!values.empty()) {
        auto comma = values.find(',');
        auto property = values.substr(0, comma);
        auto equal = property.find('=');
        properties[property.substr(0, equal)] = property.substr(equal + 1);
        if (comma == std::string_view::npos) break;
        values.remove_prefix(comma + 1);
    }
    tag["Properties"] = std::move(properties);
    return tag;
}

JavaStructure readSponge(CompoundTag const& file, std::uint64_t limit) {
    auto const& root = file.contains("Schematic") ? file["Schematic"].get<CompoundTag>() : file;
    auto version = int(root["Version"]);
    if (version < 1 || version > 3) throw std::runtime_error("unsupported Sponge schematic version");
    auto extent = [&](std::string_view name) { return int(std::uint16_t(int(root[name]))); };
    JavaStructure result;
    result.size = {extent("Width"), extent("Height"), extent("Length")};
    result.total = volume(result.size, limit);
    auto storedOffset = root.contains("Offset") ? intArray(root["Offset"]) : Vec3i{};
    if (version == 3) {
        result.offset = storedOffset;
        if (root.contains("Metadata")) {
            auto const& metadata = root["Metadata"];
            if (metadata.contains("WorldEdit") && metadata["WorldEdit"].contains("Origin")) {
                result.origin = sum(intArray(metadata["WorldEdit"]["Origin"]), result.offset);
            }
        }
    } else {
        result.origin = storedOffset;
        if (root.contains("Metadata") && root["Metadata"].contains("WEOffsetX")) {
            auto const& metadata = root["Metadata"];
            result.offset = {int(metadata["WEOffsetX"]), int(metadata["WEOffsetY"]), int(metadata["WEOffsetZ"])};
        }
    }

    auto const& data = version == 3 ? root["Blocks"].get<CompoundTag>() : root;
    auto const& palette = data["Palette"].get<CompoundTag>();
    auto const& bytes = data[version == 3 ? "Data" : "BlockData"].get<ByteArrayTag>();
    if (palette.empty() || result.total > bytes.size()) throw std::runtime_error("invalid Sponge block data");
    JavaRegion region{{}, result.size, std::vector<JavaBlock>(palette.size()), {}};
    std::vector<bool> assigned(palette.size());
    for (auto const& [state, value] : palette.mTags) {
        auto index = size_t(int(value));
        if (index >= palette.size() || assigned[index]) throw std::runtime_error("invalid Sponge palette index");
        assigned[index] = true;
        region.palette[index] = importJavaBlock(state);
    }

    region.blocks.reserve(result.total);
    size_t cursor = 0;
    for (size_t i = 0; i < result.total; ++i) {
        std::uint32_t value = 0;
        for (unsigned shift = 0;; shift += 7) {
            if (cursor >= bytes.size()) throw std::runtime_error("truncated Sponge block data");
            auto byte = bytes[cursor++];
            if (shift == 28 && byte > 0x07) throw std::runtime_error("invalid Sponge palette VarInt");
            value |= std::uint32_t(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
        }
        if (value >= region.palette.size()) throw std::runtime_error("unknown Sponge palette index");
        region.blocks.push_back(value);
    }
    if (cursor != bytes.size()) throw std::runtime_error("unexpected Sponge block data");
    result.warnings.blockEntities = listSize(data, "BlockEntities") + listSize(data, "TileEntities");
    result.warnings.actors = listSize(root, "Entities");
    result.regions.push_back(std::move(region));
    return result;
}

unsigned bitsPerBlock(size_t paletteSize) { return std::max(2u, unsigned(std::bit_width(paletteSize - 1))); }

JavaStructure readLitematic(CompoundTag const& root, std::uint64_t limit) {
    auto version = int(root["Version"]);
    if (version < 1 || version > 7) throw std::runtime_error("unsupported Litematica version");
    auto const& regions = root["Regions"].get<CompoundTag>();
    if (regions.empty()) throw std::runtime_error("Litematica file has no regions");
    JavaStructure result;
    std::optional<core::Box> bounds;
    for (auto const& [name, value] : regions.mTags) {
        auto const& tag = value.get<CompoundTag>();
        auto pos = vector(tag["Position"].get<CompoundTag>());
        auto size = vector(tag["Size"].get<CompoundTag>());
        if (!size.x || !size.y || !size.z) throw std::runtime_error("empty Litematica region");
        auto endpoint = [](int n) { return n > 0 ? n - 1 : n + 1; };
        auto box = core::Box::of(pos, sum(pos, {endpoint(size.x), endpoint(size.y), endpoint(size.z)}));
        JavaRegion region{box.min, dimensions(box), {}, {}};
        auto count = volume(region.size, limit - result.total);
        result.total += count;
        if (!bounds) bounds = box;
        else {
            bounds->min = core::Box::of(bounds->min, box.min).min;
            bounds->max = core::Box::of(bounds->max, box.max).max;
        }
        volume(dimensions(*bounds), limit);
        auto const& palette = tag["BlockStatePalette"].get<ListTag>();
        if (palette.empty()) throw std::runtime_error("empty Litematica palette");
        auto bits = bitsPerBlock(palette.size());
        auto const& bytes = tag["BlockStates"].get<ByteArrayTag>();
        auto words = (count * bits + 63) / 64;
        if (bytes.size() != words * 8) throw std::runtime_error("invalid Litematica block data length");
        region.palette.reserve(palette.size());
        for (auto const& entry : palette) region.palette.push_back(importJavaBlock(stateName(entry.get<CompoundTag>())));
        auto word = [&](size_t index) {
            std::uint64_t value = 0;
            for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[index * 8 + i];
            return value;
        };
        region.blocks.reserve(count);
        auto mask = (std::uint64_t(1) << bits) - 1;
        std::uint64_t low = word(0), high = words > 1 ? word(1) : 0;
        for (std::uint64_t i = 0, bit = 0; i < count; ++i, bit += bits) {
            auto shift = bit % 64;
            auto index = low >> shift;
            if (shift + bits > 64) index |= high << (64 - shift);
            index &= mask;
            if (index >= region.palette.size()) throw std::runtime_error("unknown Litematica palette index");
            region.blocks.push_back(std::uint32_t(index));
            if (shift + bits >= 64) {
                low = high;
                high = bit / 64 + 2 < words ? word(bit / 64 + 2) : 0;
            }
        }
        result.warnings.blockEntities += listSize(tag, "TileEntities");
        result.warnings.actors += listSize(tag, "Entities");
        result.regions.push_back(std::move(region));
    }
    result.size = dimensions(*bounds);
    for (auto& region : result.regions) region.min = region.min - bounds->min;
    return result;
}

struct ExportBlocks {
    std::vector<std::string_view> palette;
    std::vector<std::uint32_t> blocks;
    int nonAir = 0;
};

ExportBlocks mapBlocks(edit::Clipboard const& clipboard, JavaWarnings& warnings) {
    ExportBlocks result;
    result.blocks.reserve(clipboard.volume());
    result.palette.push_back(air);
    std::unordered_map<std::string_view, std::uint32_t> indexes{{air, 0}};
    struct ExportCell { std::uint32_t index = 0; bool approximate = false; };
    std::map<std::pair<Block const*, Block const*>, ExportCell> cache;
    for (size_t i = 0; i < clipboard.volume(); ++i) {
        auto const& cell = clipboard.at(i);
        auto [it, inserted] = cache.try_emplace({cell.block, cell.extra});
        auto& mapped = it->second;
        if (inserted) {
            auto java = transparent;
            if (cell.block) {
                auto const& state = clipboard.stateOf(cell.block);
                auto extra = cell.extra ? std::string_view(clipboard.stateOf(cell.extra)["name"]) : air;
                bool wet = extra == "minecraft:water" || extra == "minecraft:flowing_water";
                java = exportJavaBlock(state, wet);
                mapped.approximate = java.empty() || (extra != air && !wet);
                if (java.empty() && wet) java = exportJavaBlock(state, false);
                if (java.empty()) java = transparent;
            }
            auto [id, added] = indexes.try_emplace(java, std::uint32_t(result.palette.size()));
            if (added) result.palette.push_back(java);
            mapped.index = id->second;
        }
        result.blocks.push_back(mapped.index);
        if (mapped.approximate) ++warnings.blocks;
        if (mapped.index && result.palette[mapped.index] != transparent) ++result.nonAir;
    }
    return result;
}

CompoundTag writeSponge(edit::Clipboard const& clipboard, ExportBlocks const& data) {
    auto size = clipboard.size;
    if (std::max({size.x, size.y, size.z}) > 65535) throw std::runtime_error("Sponge dimensions exceed 65535 blocks");
    ByteArrayTag bytes;
    bytes.reserve(data.blocks.size());
    for (auto value : data.blocks) {
        while (value >= 0x80) {
            bytes.push_back(uchar((value & 0x7f) | 0x80));
            value >>= 7;
        }
        bytes.push_back(uchar(value));
    }
    CompoundTag palette;
    for (size_t i = 0; i < data.palette.size(); ++i) palette[data.palette[i]] = int(i);
    auto offset = clipboard.offset;
    auto origin = clipboard.origin;
    IntArrayTag worldEditOrigin{
        coordinate(std::int64_t(origin.x) - offset.x),
        coordinate(std::int64_t(origin.y) - offset.y),
        coordinate(std::int64_t(origin.z) - offset.z)
    };
    CompoundTag schematic{
        {"Version", 3}, {"DataVersion", javaDataVersion},
        {"Width", std::int16_t(size.x)}, {"Height", std::int16_t(size.y)}, {"Length", std::int16_t(size.z)},
        {"Offset", IntArrayTag{offset.x, offset.y, offset.z}},
        {"Metadata", CompoundTag{{"WorldEdit", CompoundTag{{"Origin", std::move(worldEditOrigin)}}}}},
        {"Blocks", CompoundTag{{"Palette", std::move(palette)}, {"Data", std::move(bytes)}}}
    };
    return {{"Schematic", std::move(schematic)}};
}

CompoundTag writeLitematic(edit::Clipboard const& clipboard, ExportBlocks const& data) {
    auto bits = bitsPerBlock(data.palette.size());
    std::vector<std::uint64_t> words((data.blocks.size() * bits + 63) / 64);
    for (size_t i = 0, bit = 0; i < data.blocks.size(); ++i, bit += bits) {
        auto value = std::uint64_t(data.blocks[i]);
        auto shift = bit % 64;
        words[bit / 64] |= value << shift;
        if (shift + bits > 64) words[bit / 64 + 1] |= value >> (64 - shift);
    }
    ByteArrayTag packed;
    packed.reserve(words.size() * 8);
    for (auto word : words) {
        for (int shift = 56; shift >= 0; shift -= 8) packed.push_back(uchar(word >> shift));
    }
    auto palette = Nbt::array();
    for (auto name : data.palette) palette.push_back(stateTag(name));
    CompoundTag region{
        {"Position", vector(Vec3i{})}, {"Size", vector(clipboard.size)},
        {"BlockStatePalette", std::move(palette)}, {"BlockStates", std::move(packed)}
    };
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CompoundTag metadata{
        {"Name", "CreatorX"}, {"Author", "CreatorX"}, {"Description", ""},
        {"RegionCount", 1}, {"TotalVolume", int(clipboard.volume())}, {"TotalBlocks", data.nonAir},
        {"EnclosingSize", vector(clipboard.size)}, {"TimeCreated", time}, {"TimeModified", time}
    };
    return {
        {"Version", 7}, {"SubVersion", 1}, {"MinecraftDataVersion", javaDataVersion},
        {"Metadata", std::move(metadata)}, {"Regions", CompoundTag{{"Region", std::move(region)}}}
    };
}

} // namespace

JavaStructure readJavaStructure(std::filesystem::path const& path, bool litematic, std::uint64_t limit) {
    auto root = readJavaNbt(path);
    limit = std::min<std::uint64_t>(limit, 256 * 1024 * 1024 / sizeof(std::uint32_t));
    return litematic ? readLitematic(root, limit) : readSponge(root, limit);
}

std::string writeJavaStructure(edit::Clipboard const& clipboard, bool litematic, JavaWarnings& warnings) {
    warnings.blockEntities = clipboard.entities.size();
    warnings.actors = clipboard.actors.size();
    auto data = mapBlocks(clipboard, warnings);
    auto root = litematic ? writeLitematic(clipboard, data) : writeSponge(clipboard, data);
    return encodeJavaNbt(root, litematic);
}

JavaImport::JavaImport(JavaStructure source) : mSource(std::move(source)), mClipboard(std::make_shared<edit::Clipboard>()) {
    mClipboard->size = mSource.size;
    mClipboard->origin = mSource.origin;
    mClipboard->offset = mSource.offset;
}

bool JavaImport::step(exec::Budget const& budget) {
    std::uint64_t used = 0;
    while (mRegion < mSource.regions.size()) {
        auto const& region = mSource.regions[mRegion];
        if (mPalette.empty()) mPalette.reserve(region.palette.size());
        while (mPalette.size() < region.palette.size()) {
            if (budget.exhausted(used)) return false;
            auto const& data = region.palette[mPalette.size()];
            auto* block = data.state ? Block::tryGetFromRegistry(*data.state).as_ptr() : nullptr;
            auto* extra = data.waterlogged ? Block::tryGetFromRegistry(HashedString("minecraft:water")).as_ptr() : nullptr;
            mPalette.push_back({{block, extra}, data.approximate || (data.state && !block) || (data.waterlogged && !extra)});
            ++used;
        }
        while (mIndex < region.blocks.size()) {
            if (budget.exhausted(used)) return false;
            auto const& cell = mPalette[region.blocks[mIndex]];
            if (cell.approximate) ++mSource.warnings.blocks;
            if (cell.blocks.block) {
                auto x = mIndex % region.size.x;
                auto line = mIndex / region.size.x;
                Vec3i local{int(x), int(line / region.size.z), int(line % region.size.z)};
                mClipboard->store(mClipboard->index(local + region.min), cell.blocks);
            }
            ++used;
            ++mIndex;
            ++mProgress;
        }
        mPalette.clear();
        mIndex = 0;
        ++mRegion;
    }
    return true;
}
} // namespace creatorx::schematic
