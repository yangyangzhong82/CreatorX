#include "schematic/Schematic.h"
#include "storage/Nbt.h"
#include "edit/Pattern.h"
#include "mod/Global.h"
#include "mc/world/level/block/Block.h"
#include <unordered_map>
#include <cmath>
#include <cctype>

namespace creatorx::schematic {
namespace {
std::optional<FileFormat> fromExtension(std::u8string extension) {
    std::ranges::transform(extension, extension.begin(), [](char8_t c) { return char8_t(std::tolower(c)); });
    if (extension == u8".cxs") return FileFormat::cxs;
    if (extension == u8".mcstructure") return FileFormat::mcstructure;
    if (extension == u8".schem") return FileFormat::schem;
    if (extension == u8".litematic") return FileFormat::litematic;
    return {};
}
} // namespace

FileFormat fileFormat(std::filesystem::path const& path) {
    auto format = fromExtension(path.extension().u8string());
    if (!format) throw std::runtime_error("unsupported schematic extension");
    return *format;
}

CompoundTag serialize(edit::Clipboard const& clipboard, bool nativeEntities) {
    using storage::Nbt;
    CompoundTag root;
    root["format_version"] = 1;
    root["size"] = storage::vector(clipboard.size);
    root["structure_world_origin"] = storage::vector(clipboard.origin);
    root["creatorx_offset"] = storage::vector(clipboard.offset);
    root["creatorx_version"] = 1;
    std::unordered_map<Block const*, int> lookup;
    auto palette = Nbt::array(), base = Nbt::array(), extra = Nbt::array();
    auto positions = Nbt::object();
    auto indexOf = [&](Block const* block) {
        if (!block) return -1;
        auto [it, added] = lookup.try_emplace(block, int(lookup.size()));
        if (added) palette.push_back(clipboard.stateOf(block));
        return it->second;
    };
    size_t index = 0;
    // 原版结构文件按 x → y → z 线性存储。
    for (int x = 0; x < clipboard.size.x; ++x) for (int y = 0; y < clipboard.size.y; ++y) for (int z = 0; z < clipboard.size.z; ++z, ++index) {
        auto local = core::Vec3i{x, y, z};
        auto source = clipboard.index(local);
        auto cell = clipboard.at(source);
        base.push_back(indexOf(cell.block));
        extra.push_back(indexOf(cell.extra));
        if (auto it = clipboard.entities.find(source); it != clipboard.entities.end()) {
            auto entity = it->second;
            if(entity.contains("pairx") && entity.contains("x")) entity["pairx"]=int(entity["pairx"])+x-int(entity["x"]);
            if(entity.contains("pairz") && entity.contains("z")) entity["pairz"]=int(entity["pairz"])+z-int(entity["z"]);
            entity["x"] = x; entity["y"] = y; entity["z"] = z;
            positions[std::to_string(index)] = Nbt::object({{"block_entity_data", std::move(entity)}});
        }
    }
    auto actors=Nbt::array();
    for (auto const& actor : clipboard.actors) {
        if (!nativeEntities) {
            actors.push_back(actor);
            continue;
        }
        auto tag = actor["nbt"].get<CompoundTag>();
        auto const& pos = actor["pos"].get<ListTag>();
        tag["Pos"] = Nbt::array({
            float(double(pos[0]) + clipboard.origin.x),
            float(double(pos[1]) + clipboard.origin.y),
            float(double(pos[2]) + clipboard.origin.z)
        });
        actors.push_back(std::move(tag));
    }
    auto& structure = root["structure"] = Nbt::object();
    auto& indices = structure["block_indices"] = Nbt::array();
    indices.push_back(std::move(base));
    indices.push_back(std::move(extra));
    structure["entities"] = std::move(actors);
    structure["palette"] = Nbt::object();
    auto& data = structure["palette"]["default"] = Nbt::object();
    data["block_palette"] = std::move(palette);
    data["block_position_data"] = std::move(positions);
    return root;
}

std::shared_ptr<edit::Clipboard const> deserialize(CompoundTag const& root) {
    if (int(root["format_version"]) != 1) throw std::runtime_error("unsupported structure version");
    auto result = std::make_shared<edit::Clipboard>();
    result->size = storage::vector(root["size"]);
    auto size = result->size;
    if (size.x <= 0 || size.y <= 0 || size.z <= 0 || !core::CuboidSize{std::uint64_t(size.x), std::uint64_t(size.y), std::uint64_t(size.z)}.fits(config.limits.maxSelectionVolume)) {
        throw std::runtime_error("structure size exceeds selection limit");
    }
    result->origin = root.contains("structure_world_origin") ? storage::vector(root["structure_world_origin"]) : core::Vec3i{};
    result->offset = root.contains("creatorx_offset") ? storage::vector(root["creatorx_offset"]) : core::Vec3i{};
    auto const& structure = root["structure"];
    if(structure.contains("entities")) for(auto const& actor:structure["entities"].get<ListTag>()) {
        auto entry=actor.get<CompoundTag>();
        // 旧版 CreatorX 的实体包装已使用局部坐标；原版 Pos 是保存时的世界坐标。
        if (!entry.contains("nbt") || !entry.contains("pos")) {
            auto const& world = entry["Pos"].get<ListTag>();
            if (world.size() != 3) throw std::runtime_error("invalid entity position in structure");
            CompoundTag wrapped;
            wrapped["pos"] = storage::Nbt::array({
                double(world[0]) - result->origin.x,
                double(world[1]) - result->origin.y,
                double(world[2]) - result->origin.z
            });
            wrapped["nbt"] = std::move(entry);
            entry = std::move(wrapped);
        }
        auto const& pos=entry["pos"].get<ListTag>();
        if(pos.size()!=3 || !entry.contains("nbt")) throw std::runtime_error("invalid entity in structure");
        for(size_t axis=0;axis<3;++axis) {
            double coordinate=double(pos[axis]);
            int extent=axis==0 ? size.x : axis==1 ? size.y : size.z;
            if(!std::isfinite(coordinate) || coordinate<0 || coordinate>extent) throw std::runtime_error("entity lies outside structure");
        }
        result->actors.push_back(std::move(entry));
    }
    auto const& data = structure["palette"]["default"];
    std::vector<Block const*> palette;
    for (auto const& tag : data["block_palette"].get<ListTag>()) {
        auto block = Block::tryGetFromRegistry(tag.get<CompoundTag>());
        if (!block) throw std::runtime_error("unknown block in structure");
        palette.push_back(block.as_ptr());
    }
    auto const& indices = structure["block_indices"].get<ListTag>();
    if (indices.size() != 2) throw std::runtime_error("invalid structure layers");
    auto const& base = indices[0].get<ListTag>();
    auto const& extra = indices[1].get<ListTag>();
    if (base.size() != result->volume() || extra.size() != result->volume()) throw std::runtime_error("invalid structure block count");
    auto blockAt = [&](int index) -> Block const* {
        if (index == -1) return nullptr;
        if (index < 0 || size_t(index) >= palette.size()) throw std::runtime_error("invalid structure palette index");
        return palette[index];
    };
    size_t index = 0;
    for (int x = 0; x < size.x; ++x) for (int y = 0; y < size.y; ++y) for (int z = 0; z < size.z; ++z, ++index) {
        auto target = result->index({x, y, z});
        result->store(target, {blockAt(int(base[index])), blockAt(int(extra[index]))});
        auto key = std::to_string(index);
        if (data.contains("block_position_data") && data["block_position_data"].contains(key)) {
            auto const& position = data["block_position_data"][key];
            if (position.contains("block_entity_data")) result->entities.emplace(target, position["block_entity_data"].get<CompoundTag>());
        }
    }
    return result;
}

std::filesystem::path path(std::string name, std::string format) {
    if (name.empty() || name.back() == '.' || name.back() == ' '
        || name.find_first_of("/\\:\"<>|?*") != std::string::npos
        || std::ranges::any_of(name, [](unsigned char c) { return c < 32; })) {
        throw std::runtime_error("invalid schematic name");
    }
    if (!fromExtension(u8"." + std::u8string(format.begin(), format.end()))) throw std::runtime_error("unsupported schematic format");
    auto p = std::filesystem::path(std::u8string(name.begin(),name.end()));
    if (!p.has_extension()) p += "." + format;
    fileFormat(p);
    return storage::dataDir() / "schematics" / p;
}

std::vector<std::string> listFiles(std::string_view search) {
    auto directory = storage::dataDir() / "schematics";
    std::filesystem::create_directories(directory);
    std::vector<std::string> names;
    std::string needle(search);
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (auto const& item : std::filesystem::directory_iterator(directory)) {
        if (!item.is_regular_file()) continue;
        if (!fromExtension(item.path().extension().u8string())) continue;
        auto raw = item.path().filename().u8string();
        std::string name(raw.begin(), raw.end());
        auto folded = name;
        std::ranges::transform(folded, folded.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (folded.find(needle) != std::string::npos) names.push_back(std::move(name));
    }
    std::ranges::sort(names);
    return names;
}

std::string list(std::string_view search) {
    std::string result;
    for (auto const& name : listFiles(search)) result += name + "\n";
    return result;
}
} // namespace creatorx::schematic
