#pragma once

#include "core/math/Vec3i.h"

class Player;

namespace creatorx {

namespace core {
struct CuboidSelection;
}

void showSelectionOutline(Player const& player, core::CuboidSelection const& selection, bool preview = false);
void clearSelectionOutline(Player const& player, bool preview = false);
void showBrushTarget(Player const& player, core::Vec3i position);

} // namespace creatorx
