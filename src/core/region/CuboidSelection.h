#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace creatorx::core {

struct Vec3i {
    int x, y, z;

    bool operator==(Vec3i const&) const = default;
};

enum class Direction { me, north, south, east, west, up, down };

struct CuboidSize {
    std::uint64_t x, y, z;

    bool          fits(std::uint64_t limit) const { return x <= limit / y / z; }
    std::uint64_t volume() const { return x * y * z; }
};

struct CuboidSelection {
    std::optional<Vec3i> first;
    std::optional<Vec3i> second;

    std::optional<CuboidSize> size() const {
        if (!first || !second) return std::nullopt;
        auto extent = [](int a, int b) -> std::uint64_t {
            return static_cast<std::int64_t>(std::max(a, b)) - std::min(a, b) + 1;
        };
        return CuboidSize{extent(first->x, second->x), extent(first->y, second->y), extent(first->z, second->z)};
    }

    bool resize(Direction direction, int amount, int reverseAmount, bool contract) {
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
        bool ordered = first->y <= second->y;
        first->y     = ordered ? minY : maxY;
        second->y    = ordered ? maxY : minY;
    }
};

} // namespace creatorx::core
