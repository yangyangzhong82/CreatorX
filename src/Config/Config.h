#pragma once

#include "Reflectable.h"
#include "ll/api/io/LogLevel.h"
#include <cstdint>
#include <string>

namespace creatorx {

struct CommandConfig {
    bool shortCommands   = true;
    bool chatDoubleSlash = true;
};

struct LimitConfig {
    std::uint64_t maxSelectionVolume = 8000000;
};

struct SelectionConfig {
    float traceDistance = 128.0f;
};

struct WandConfig {
    std::string item       = "minecraft:wooden_axe";
    bool        requireTag = true;
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
    LimitConfig      limits{};
    SelectionConfig  selection{};
    WandConfig       wand{};
    VisualConfig     visual{};
};

template <>
struct is_reflectable<CommandConfig> : std::true_type {};
template <>
struct is_reflectable<LimitConfig> : std::true_type {};
template <>
struct is_reflectable<SelectionConfig> : std::true_type {};
template <>
struct is_reflectable<WandConfig> : std::true_type {};
template <>
struct is_reflectable<VisualConfig> : std::true_type {};
template <>
struct is_reflectable<Config> : std::true_type {};

} // namespace creatorx
