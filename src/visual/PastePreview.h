#pragma once
#include "edit/Clipboard.h"
#include "mc/platform/UUID.h"
class Player;
namespace creatorx::edit { class Mask; }
namespace creatorx {
std::uint64_t showPastePreview(Player& player, edit::Clipboard const& clipboard, core::Vec3i min, bool skipAir, edit::Mask const* mask = nullptr);
bool finishPastePreview(Player& player, std::uint64_t token);
void cancelPastePreview(Player& player);
void enablePreviews();
void disablePreviews();
}
