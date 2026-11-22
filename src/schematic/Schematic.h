#pragma once
#include "edit/Clipboard.h"
#include <filesystem>
#include <string>

namespace creatorx::schematic {
CompoundTag serialize(edit::Clipboard const& clipboard, bool nativeEntities = false);
std::shared_ptr<edit::Clipboard const> deserialize(CompoundTag const& tag);
std::filesystem::path path(std::string name, std::string format = "cxs");
std::string list(std::string_view search = {});
} // namespace creatorx::schematic
