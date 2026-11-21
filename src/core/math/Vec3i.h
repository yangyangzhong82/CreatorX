#pragma once

namespace creatorx::core {

struct Vec3i {
    int x, y, z;

    bool operator==(Vec3i const&) const = default;

    Vec3i operator+(Vec3i const& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3i operator-(Vec3i const& other) const { return {x - other.x, y - other.y, z - other.z}; }
};

} // namespace creatorx::core
