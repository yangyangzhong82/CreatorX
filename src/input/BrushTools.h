#pragma once

#include "edit/Pattern.h"

class ItemStack;
class Player;

namespace creatorx {

enum class BrushTool { sphere, cyl, smooth, clipboard, gravity, info, repl };

struct BrushBinding {
    BrushTool                    tool;
    std::optional<edit::Pattern> pattern;
    std::optional<edit::Mask>    mask;
    int                          radius     = 3;
    int                          height     = 1;
    int                          iterations = 1;
    int                          range      = 32;
    bool                         hollow     = false;
    bool                         skipAir    = false;
    bool                         original   = false;
};

void enableBrushTools();
void disableBrushTools();
bool handleBrushClick(Player& player, ItemStack const& item, bool left);

} // namespace creatorx
