#pragma once

#include "core/math/Vec3i.h"
#include "core/region/CuboidSelection.h"

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
    std::vector<CompoundTag> actors;

    std::size_t index(core::Vec3i local) const {
        return (std::size_t(local.y) * std::size_t(size.z) + std::size_t(local.z)) * std::size_t(size.x)
             + std::size_t(local.x);
    }
    std::uint64_t volume() const { return std::uint64_t(size.x) * std::uint64_t(size.y) * std::uint64_t(size.z); }

    // 复制任务实际读到该页时才分配，完整复制后才会向粘贴任务公开。
    void store(std::size_t index, Cell cell) {
        rememberStates(cell);
        if (mPages.empty()) mPages.resize((volume() + pageSize - 1) / pageSize);
        auto  pageIndex = index / pageSize;
        auto& page      = mPages[pageIndex];
        if (!page) {
            auto count = std::min<std::size_t>(pageSize, volume() - pageIndex * pageSize);
            page = std::make_unique<Cell[]>(count);
        }
        page[index % pageSize] = cell;
    }

    Cell const& at(std::size_t index) const {
        static constexpr Cell empty{nullptr, nullptr};
        if (mPages.empty()) return empty;
        auto const& page = mPages[index / pageSize];
        return page ? page[index % pageSize] : empty;
    }
    CompoundTag const& stateOf(Block const* block) const { return mStates.at(block); }

private:
    void rememberStates(Cell cell);
    std::unordered_map<Block const*, CompoundTag> mStates;
    static constexpr std::size_t         pageSize = 1024;
    std::vector<std::unique_ptr<Cell[]>> mPages;
};

// 对剪贴板做坐标和方块状态变换。旋转以指定轴的 90 度为单位，翻转在对应轴上镜像。
std::shared_ptr<Clipboard const> rotateClipboard(std::shared_ptr<Clipboard const> const& source, int quarterTurns, int axis = 1);
std::shared_ptr<Clipboard const> flipClipboard(
    std::shared_ptr<Clipboard const> const& source,
    core::Direction direction
);

} // namespace creatorx::edit
