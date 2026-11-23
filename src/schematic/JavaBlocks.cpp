#include "schematic/JavaBlocks.h"
#include "schematic/JavaNbt.h"
#include "mod/Entry.h"
#include <algorithm>
#include <map>

namespace creatorx::schematic {
namespace {
bool waterlogged(std::string_view state) {
    return state.find("[waterlogged=true") != std::string_view::npos
        || state.find(",waterlogged=true") != std::string_view::npos
        || state == "minecraft:bubble_column[drag=false]" || state == "minecraft:bubble_column[drag=true]"
        || state.starts_with("minecraft:kelp[") || state == "minecraft:kelp_plant"
        || state.starts_with("minecraft:seagrass") || state.starts_with("minecraft:tall_seagrass[");
}

std::string bedrockKey(CompoundTag const& state) {
    CompoundTag key{{"name", state["name"]}, {"states", state["states"]}};
    return key.toBinaryNbt();
}

struct JavaBlocks {
    struct Entry {
        std::string name;
        JavaBlock block;
    };

    CompoundTag data;
    std::vector<Entry> entries;
    std::unordered_map<std::string_view, JavaBlock> java;
    std::unordered_map<std::string_view, JavaBlock> defaults;
    std::map<std::pair<std::string, bool>, std::string_view> bedrock;

    JavaBlocks() : data(readJavaNbt(creatorx::Entry::getInstance().getSelf().getModDir() / "assets" / "java-blocks.nbt.gz")) {
        auto const& names = data["Java"].get<ListTag>();
        auto const& indexes = data["Index"].get<IntArrayTag>();
        auto const& isDefault = data["Default"].get<ByteArrayTag>();
        auto const& states = data["Bedrock"].get<ListTag>();
        std::vector<std::string> keys;
        keys.reserve(states.size());
        for (auto const& state : states) keys.push_back(bedrockKey(state.get<CompoundTag>()));
        entries.reserve(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            auto name = std::string(std::string_view(names[i]));
            auto index = indexes.at(i);
            auto const& state = states.at(index).get<CompoundTag>();
            auto wet = waterlogged(name);
            auto const& entry = entries.emplace_back(JavaBlocks::Entry{std::move(name), {&state, wet}});
            java.emplace(entry.name, entry.block);
            if (isDefault.at(i)) defaults.emplace(std::string_view(entry.name).substr(0, entry.name.find('[')), entry.block);
            bedrock.try_emplace({keys.at(index), wet}, entry.name);
        }
    }
};

JavaBlocks const& mappings() {
    static JavaBlocks const result;
    return result;
}
std::string canonicalJavaState(std::string_view state) {
    auto bracket = state.find('[');
    auto name = state.substr(0, bracket);
    std::string canonical;
    if (!name.contains(':')) canonical = "minecraft:";
    canonical += name;
    if (bracket == std::string_view::npos) return canonical;
    if (!state.ends_with(']')) throw std::runtime_error("invalid Java block state");
    auto properties = state.substr(bracket + 1, state.size() - bracket - 2);
    std::vector<std::string_view> sorted;
    while (!properties.empty()) {
        auto comma = properties.find(',');
        sorted.push_back(properties.substr(0, comma));
        if (comma == std::string_view::npos) break;
        properties.remove_prefix(comma + 1);
    }
    std::ranges::sort(sorted);
    for (size_t i = 0; i < sorted.size(); ++i) {
        canonical += i ? ',' : '[';
        canonical += sorted[i];
    }
    if (!sorted.empty()) canonical += ']';
    return canonical;
}
} // namespace

JavaBlock importJavaBlock(std::string_view state) {
    auto key = canonicalJavaState(state);
    if (key == "minecraft:structure_void") return {};
    auto const& table = mappings();
    if (auto found = table.java.find(key); found != table.java.end()) return found->second;
    auto bracket = key.find('[');
    if (auto found = table.defaults.find(std::string_view(key).substr(0, bracket)); found != table.defaults.end()) {
        auto block = found->second;
        block.approximate = bracket != std::string::npos;
        return block;
    }
    return {nullptr, false, true};
}

std::string_view exportJavaBlock(CompoundTag const& state, bool waterlogged) {
    auto const& table = mappings().bedrock;
    if (auto found = table.find({bedrockKey(state), waterlogged}); found != table.end()) return found->second;
    return {};
}
} // namespace creatorx::schematic
