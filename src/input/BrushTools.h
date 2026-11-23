#pragma once

#include "edit/Pattern.h"
#include "command/Feedback.h"

class ItemStack;
class Player;

namespace creatorx {

enum class BrushTool { sphere, cyl, smooth, clipboard, gravity, info, repl };

struct BrushBinding {
    BrushTool                    tool;
    std::optional<edit::Pattern> pattern;
    std::optional<edit::Mask>    mask;
    std::string                  patternText;
    std::string                  maskText;
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

// GUI and other non-command callers use the same parser as the command handlers.
void executeBrush(Player& player, Feedback const& output, std::string_view action, std::string_view arguments);
void executeTool(Player& player, Feedback const& output, std::string_view action, std::string_view arguments);
void executeBrushOption(Player& player, Feedback const& output, std::string_view option, std::string_view arguments);

} // namespace creatorx
