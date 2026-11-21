#pragma once

#include "Reflectable.h"
#include "ll/api/io/LogLevel.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace creatorx {

struct CommandConfig {
    bool shortCommands = true;
};

struct LimitConfig {
    std::uint64_t            maxSelectionVolume     = 8000000;
    std::uint64_t            maxChangesPerOperation = 4000000;
    std::uint64_t            confirmThreshold       = 1000000;
    std::vector<std::string> disallowedBlocks{
        "minecraft:command_block",
        "minecraft:chain_command_block",
        "minecraft:repeating_command_block",
        "minecraft:structure_block",
        "minecraft:barrier"
    };
};

struct PerformanceConfig {
    std::uint64_t blocksPerTick     = 20000;
    float         tickBudgetMs      = 15.0f;
    unsigned      maxConcurrentJobs = 4;
    bool          physics           = false;
};

struct HistoryConfig {
    std::size_t size              = 30;
    std::size_t maxBlocksInMemory = 5000000;
};

struct SelectionConfig {
    float traceDistance = 128.0f;
};

struct WandConfig {
    std::string item           = "minecraft:wooden_axe";
    bool        requireTag     = true;
    bool        sneakOpensMenu = true;
};

struct VisualConfig {
    std::string mode        = "auto";
    float       maxDistance = 128.0f;
    std::string particle    = "minecraft:villager_happy";
};

struct Config {
    int              version  = 1;
    ll::io::LogLevel logLevel = ll::io::LogLevel::Info;
    std::string      language = "zh_CN";
    CommandConfig    command{};
    LimitConfig       limits{};
    PerformanceConfig performance{};
    HistoryConfig     history{};
    SelectionConfig   selection{};
    WandConfig        wand{};
    VisualConfig      visual{};
};

template <>
struct is_reflectable<CommandConfig> : std::true_type {};
template <>
struct is_reflectable<LimitConfig> : std::true_type {};
template <>
struct is_reflectable<PerformanceConfig> : std::true_type {};
template <>
struct is_reflectable<HistoryConfig> : std::true_type {};
template <>
struct is_reflectable<SelectionConfig> : std::true_type {};
template <>
struct is_reflectable<WandConfig> : std::true_type {};
template <>
struct is_reflectable<VisualConfig> : std::true_type {};
template <>
struct is_reflectable<Config> : std::true_type {};

} // namespace creatorx
