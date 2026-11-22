#pragma once

#include "core/math/Vec3i.h"
#include "mc/deps/nbt/CompoundTag.h"
#include <filesystem>
#include <string>
#include <future>
#include <functional>
#include <variant>

namespace creatorx::storage {
using Nbt = CompoundTagVariant;
inline Nbt vector(core::Vec3i p) { return Nbt::array({p.x, p.y, p.z}); }
inline core::Vec3i vector(Nbt const& value) {
    auto const& list = value.get<ListTag>();
    if (list.size() != 3) throw std::runtime_error("invalid vector");
    return {int(list[0]), int(list[1]), int(list[2])};
}
std::filesystem::path dataDir();
CompoundTag readNbt(std::filesystem::path const& path);
void writeNbt(std::filesystem::path const& path, CompoundTag const& tag);
using NbtSource = std::variant<CompoundTag, std::function<CompoundTag()>>;
std::shared_future<void> queueNbt(std::filesystem::path path, NbtSource source, std::vector<std::shared_future<void>> dependencies = {});
void flushWrites();
void stopStorage();
void startStorage();
} // namespace creatorx::storage
