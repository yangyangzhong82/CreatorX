#pragma once

#include "core/math/Vec3i.h"

#include <algorithm>
#include <cstdint>
#include <functional>

namespace creatorx::core {

// 闭区间包围盒。
struct Box {
    Vec3i min, max;
    // 非长方体选区的成员判断；空函数表示完整包围盒。
    std::function<bool(Vec3i)> region;

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
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z
            && (!region || region(p));
    }

    Box shifted(Vec3i offset) const {
        Box result{min + offset, max + offset};
        if (region) result.region = [test = region, offset](Vec3i p) { return test(p - offset); };
        return result;
    }
};

// 区域操作只作用于包围盒的哪一部分。
enum class BoxShape { all, walls, faces };

inline bool inShape(Box const& box, Vec3i p, BoxShape shape) {
    if (!box.contains(p)) return false;
    if (shape == BoxShape::all) return true;
    if (box.region) {
        bool side = !box.contains(p + Vec3i{1, 0, 0}) || !box.contains(p + Vec3i{-1, 0, 0})
            || !box.contains(p + Vec3i{0, 0, 1}) || !box.contains(p + Vec3i{0, 0, -1});
        return side || (shape == BoxShape::faces
            && (!box.contains(p + Vec3i{0, 1, 0}) || !box.contains(p + Vec3i{0, -1, 0})));
    }
    bool side = p.x == box.min.x || p.x == box.max.x || p.z == box.min.z || p.z == box.max.z;
    if (shape == BoxShape::walls) return side;
    return side || p.y == box.min.y || p.y == box.max.y;
}

inline std::uint64_t shapeVolume(Box const& box, BoxShape shape) {
    if (box.region) return box.volume(); // 调度和改动上限使用包围盒估计，不同步扫描大选区。
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
    explicit BoxIterator(Box const& box, BoxShape shape = BoxShape::all)
    : mBox(box), mShape(shape), mChunkX(box.min.x >> 4), mChunkZ(box.min.z >> 4) {
        enterColumn();
    }

    Vec3i const* current() const { return mDone ? nullptr : &mCurrent; }
    bool includes(Vec3i p) const { return inShape(mBox, p, mShape); }

    void advance() {
        advanceOne();
        if (mShape != BoxShape::all && !mBox.region) seekSurface();
    }

private:
    void advanceOne() {
        if (++mCurrent.x <= mMaxX) return;
        mCurrent.x = mMinX;
        if (++mCurrent.z <= mMaxZ) return;
        mCurrent.z = mMinZ;
        if (++mCurrent.y <= mBox.max.y) return;
        nextColumn();
    }

    void nextColumn() {
        // 围墙的中间区块行只有东西两端需要访问。
        if (!mBox.region && mShape == BoxShape::walls && mChunkZ > (mBox.min.z >> 4) && mChunkZ < (mBox.max.z >> 4)
            && mChunkX < (mBox.max.x >> 4)) {
            mChunkX = mBox.max.x >> 4;
            enterColumn();
            return;
        }
        if (++mChunkX > (mBox.max.x >> 4)) {
            mChunkX = mBox.min.x >> 4;
            if (++mChunkZ > (mBox.max.z >> 4)) {
                mDone = true;
                return;
            }
        }
        enterColumn();
    }

    void seekSurface() {
        while (!mDone && !inShape(mBox, mCurrent, mShape)) {
            // 保持 y → z → x 的原有顺序，跳到本行、本层或本列的下一个边界。
            if (mMaxX == mBox.max.x) {
                mCurrent.x = mMaxX;
                return;
            }
            mCurrent.x = mMinX;
            if (mMinX == mBox.min.x && mCurrent.z < mMaxZ) {
                ++mCurrent.z;
                return;
            }
            if (mMaxZ == mBox.max.z) {
                mCurrent.z = mMaxZ;
                return;
            }
            mCurrent.z = mMinZ;
            if (mCurrent.y < mBox.max.y) {
                if (mMinX == mBox.min.x || mMinZ == mBox.min.z) {
                    ++mCurrent.y;
                    return;
                }
                if (mShape == BoxShape::faces) {
                    mCurrent.y = mBox.max.y;
                    return;
                }
            }
            nextColumn();
        }
    }

    void enterColumn() {
        mMinX    = std::max(mBox.min.x, mChunkX * 16);
        mMaxX    = std::min(mBox.max.x, mChunkX * 16 + 15);
        mMinZ    = std::max(mBox.min.z, mChunkZ * 16);
        mMaxZ    = std::min(mBox.max.z, mChunkZ * 16 + 15);
        mCurrent = {mMinX, mBox.min.y, mMinZ};
    }

    Box   mBox;
    BoxShape mShape;
    int   mChunkX, mChunkZ;
    int   mMinX{}, mMaxX{}, mMinZ{}, mMaxZ{};
    Vec3i mCurrent{};
    bool  mDone = false;
};

} // namespace creatorx::core
