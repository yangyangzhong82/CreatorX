#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace creatorx::core {

struct Vec3i {
    int x, y, z;
};

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
};

} // namespace creatorx::core
