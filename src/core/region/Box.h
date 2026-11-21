#pragma once

#include "core/math/Vec3i.h"

#include <algorithm>
#include <cstdint>

namespace creatorx::core {

// 闭区间包围盒。
struct Box {
    Vec3i min, max;

    static Box of(Vec3i a, Vec3i b) {
        return {
            {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
            {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}
        };
    }

    Vec3i size() const { return {max.x - min.x + 1, max.y - min.y + 1, max.z - min.z + 1}; }

    std::uint64_t volume() const {
        auto s = size();
        return std::uint64_t(s.x) * std::uint64_t(s.y) * std::uint64_t(s.z);
    }

    bool contains(Vec3i p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }

    Box shifted(Vec3i offset) const { return {min + offset, max + offset}; }
};

// 区域操作只作用于包围盒的哪一部分。
enum class BoxShape { all, walls, faces };

inline bool inShape(Box const& box, Vec3i p, BoxShape shape) {
    if (shape == BoxShape::all) return true;
    bool side = p.x == box.min.x || p.x == box.max.x || p.z == box.min.z || p.z == box.max.z;
    if (shape == BoxShape::walls) return side;
    return side || p.y == box.min.y || p.y == box.max.y;
}

inline std::uint64_t shapeVolume(Box const& box, BoxShape shape) {
    auto s     = box.size();
    auto inner = [](int n) { return std::uint64_t(std::max(n - 2, 0)); };
    switch (shape) {
    case BoxShape::all:
        return box.volume();
    case BoxShape::walls:
        return box.volume() - inner(s.x) * std::uint64_t(s.y) * inner(s.z);
    case BoxShape::faces:
        return box.volume() - inner(s.x) * inner(s.y) * inner(s.z);
    }
    return 0;
}

// 按 区块列 → y → z → x 的顺序遍历包围盒，同一区块的写入集中在一起。
class BoxIterator {
public:
    explicit BoxIterator(Box const& box) : mBox(box), mChunkX(box.min.x >> 4), mChunkZ(box.min.z >> 4) {
        enterColumn();
    }

    bool next(Vec3i& out) {
        if (mDone) return false;
        out = mCurrent;
        if (++mCurrent.x <= mMaxX) return true;
        mCurrent.x = mMinX;
        if (++mCurrent.z <= mMaxZ) return true;
        mCurrent.z = mMinZ;
        if (++mCurrent.y <= mBox.max.y) return true;
        if (++mChunkX > (mBox.max.x >> 4)) {
            mChunkX = mBox.min.x >> 4;
            if (++mChunkZ > (mBox.max.z >> 4)) {
                mDone = true;
                return true;
            }
        }
        enterColumn();
        return true;
    }

private:
    void enterColumn() {
        mMinX    = std::max(mBox.min.x, mChunkX * 16);
        mMaxX    = std::min(mBox.max.x, mChunkX * 16 + 15);
        mMinZ    = std::max(mBox.min.z, mChunkZ * 16);
        mMaxZ    = std::min(mBox.max.z, mChunkZ * 16 + 15);
        mCurrent = {mMinX, mBox.min.y, mMinZ};
    }

    Box   mBox;
    int   mChunkX, mChunkZ;
    int   mMinX{}, mMaxX{}, mMinZ{}, mMaxZ{};
    Vec3i mCurrent{};
    bool  mDone = false;
};

} // namespace creatorx::core
