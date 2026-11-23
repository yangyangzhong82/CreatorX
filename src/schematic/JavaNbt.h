#pragma once

#include "mc/deps/nbt/CompoundTag.h"
#include <filesystem>
#include <string>

namespace creatorx::schematic {
CompoundTag readJavaNbt(std::filesystem::path const& path);
std::string encodeJavaNbt(CompoundTag const& root, bool litematic);
}
