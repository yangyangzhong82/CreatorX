#pragma once

#include "edit/Clipboard.h"
#include <string_view>

namespace creatorx::schematic {
struct JavaBlock {
    CompoundTag const* state = nullptr;
    bool waterlogged = false;
    bool approximate = false;
};

JavaBlock importJavaBlock(std::string_view state);
std::string_view exportJavaBlock(CompoundTag const& state, bool waterlogged);
inline constexpr int javaDataVersion = 5023; // Java 26.3
}
