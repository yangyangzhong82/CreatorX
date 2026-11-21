#pragma once

#include "core/math/Vec3i.h"

#include "mc/deps/nbt/CompoundTag.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class Block;

namespace creatorx::edit {

struct Clipboard {
    struct Cell {
        Block const* block;
        Block const* extra;
    };

    core::Vec3i               size;
    core::Vec3i               offset; // 最小角相对复制者脚下方块的偏移
    core::Vec3i               origin; // 复制时最小角的世界坐标（paste -o 使用）
    std::unordered_map<std::size_t, CompoundTag> entities;

    std::size_t index(core::Vec3i local) const {
        return (std::size_t(local.y) * std::size_t(size.z) + std::size_t(local.z)) * std::size_t(size.x)
             + std::size_t(local.x);
    }
    std::uint64_t volume() const { return std::uint64_t(size.x) * std::uint64_t(size.y) * std::uint64_t(size.z); }

    // 复制任务实际读到该页时才分配，完整复制后才会向粘贴任务公开。
    void store(std::size_t index, Cell cell) {
        if (mPages.empty()) mPages.resize((volume() + pageSize - 1) / pageSize);
        auto  pageIndex = index / pageSize;
        auto& page      = mPages[pageIndex];
        if (!page) {
            auto count = std::min<std::size_t>(pageSize, volume() - pageIndex * pageSize);
            page = std::make_unique_for_overwrite<Cell[]>(count);
        }
        page[index % pageSize] = cell;
    }

    Cell const& at(std::size_t index) const { return mPages[index / pageSize][index % pageSize]; }

private:
    static constexpr std::size_t         pageSize = 1024;
    std::vector<std::unique_ptr<Cell[]>> mPages;
};

} // namespace creatorx::edit
