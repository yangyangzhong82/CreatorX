#pragma once

#include "core/math/Vec3i.h"
#include "core/region/Box.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include <array>

namespace creatorx::core {

enum class Direction { me, north, south, east, west, up, down };
enum class SelectionType { cuboid, poly, ellipsoid, sphere, cyl, convex };

struct CuboidSize {
    std::uint64_t x, y, z;

    bool          fits(std::uint64_t limit) const { return x <= limit / y / z; }
    std::uint64_t volume() const { return x * y * z; }
};

struct CuboidSelection {
    std::optional<Vec3i> first;
    std::optional<Vec3i> second;
    SelectionType type = SelectionType::cuboid;
    std::vector<Vec3i> points;
    Vec3i radii{};
    std::optional<std::pair<int,int>> height;
    // 凸包三角面，保存顶点索引供包含判断和轮廓共用。
    std::vector<std::array<size_t, 3>> faces;

    void setPoint(bool primary, Vec3i point);
    void rebuildHull();
    bool complete() const;
    bool contains(Vec3i point) const;
    void shift(Vec3i offset);
    std::optional<Box> bounds() const;

    std::optional<CuboidSize> size() const {
        auto bounds = this->bounds();
        if (!bounds) return std::nullopt;
        auto extent = [](int a, int b) -> std::uint64_t {
            return static_cast<std::int64_t>(std::max(a, b)) - std::min(a, b) + 1;
        };
        return CuboidSize{extent(bounds->min.x, bounds->max.x), extent(bounds->min.y, bounds->max.y), extent(bounds->min.z, bounds->max.z)};
    }

    std::optional<Box> box() const {
        auto result = bounds();
        if (result && type != SelectionType::cuboid) result->region = [selection = *this](Vec3i p) { return selection.contains(p); };
        return result;
    }

    bool resize(Direction direction, int amount, int reverseAmount, bool contract) {
        if(type != SelectionType::cuboid) return false;
        int Vec3i::* axis;
        switch (direction) {
        case Direction::east:
        case Direction::west:
            axis = &Vec3i::x;
            break;
        case Direction::up:
        case Direction::down:
            axis = &Vec3i::y;
            break;
        case Direction::north:
        case Direction::south:
            axis = &Vec3i::z;
            break;
        case Direction::me:
            return false;
        }

        auto& a          = (*first).*axis;
        auto& b          = (*second).*axis;
        auto [low, high] = std::minmax(a, b);
        bool positive    = direction == Direction::east || direction == Direction::up || direction == Direction::south;
        int  lowerAmount = positive ? reverseAmount : amount;
        int  upperAmount = positive ? amount : reverseAmount;

        auto newLow  = static_cast<std::int64_t>(low);
        auto newHigh = static_cast<std::int64_t>(high);
        if (contract) {
            newLow  += upperAmount;
            newHigh -= lowerAmount;
        } else {
            newLow  -= lowerAmount;
            newHigh += upperAmount;
        }
        if (newLow > newHigh || newLow < std::numeric_limits<int>::min() || newHigh > std::numeric_limits<int>::max()) {
            return false;
        }

        bool ordered = a <= b;
        a            = static_cast<int>(ordered ? newLow : newHigh);
        b            = static_cast<int>(ordered ? newHigh : newLow);
        return true;
    }

    void expandVertical(int minY, int maxY) {
        if (type == SelectionType::poly || type == SelectionType::cyl) {
            height=std::pair{minY,maxY};
            return;
        }
        bool ordered = first->y <= second->y;
        first->y     = ordered ? minY : maxY;
        second->y    = ordered ? maxY : minY;
    }
};

} // namespace creatorx::core
