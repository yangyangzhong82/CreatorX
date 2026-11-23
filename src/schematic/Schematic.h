#pragma once
#include "edit/Clipboard.h"
#include <filesystem>
#include <string>

namespace creatorx::schematic {
enum class FileFormat { cxs, mcstructure, schem, litematic };
FileFormat fileFormat(std::filesystem::path const& path);
CompoundTag serialize(edit::Clipboard const& clipboard, bool nativeEntities = false);
std::shared_ptr<edit::Clipboard const> deserialize(CompoundTag const& tag);
std::filesystem::path path(std::string name, std::string format = "cxs");
std::vector<std::string> listFiles(std::string_view search = {});
std::string list(std::string_view search = {});
} // namespace creatorx::schematic
