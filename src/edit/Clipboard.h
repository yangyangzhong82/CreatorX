#pragma once

#include "core/math/Vec3i.h"

#include "mc/deps/nbt/CompoundTag.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class Block;

namespace creatorx::edit {

struct Clipboard {
    core::Vec3i               size;
    core::Vec3i               offset; // 最小角相对复制者脚下方块的偏移
    core::Vec3i               origin; // 复制时最小角的世界坐标（paste -o 使用）
    std::vector<Block const*> blocks; // 按 y → z → x 排列
    std::vector<Block const*> extras;
    std::unordered_map<std::size_t, CompoundTag> entities;

    std::size_t index(core::Vec3i local) const {
        return (std::size_t(local.y) * std::size_t(size.z) + std::size_t(local.z)) * std::size_t(size.x)
             + std::size_t(local.x);
    }
    std::uint64_t volume() const { return std::uint64_t(size.x) * std::uint64_t(size.y) * std::uint64_t(size.z); }
};

} // namespace creatorx::edit
