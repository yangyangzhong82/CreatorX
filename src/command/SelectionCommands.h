#pragma once

#include "command/Feedback.h"
#include "core/region/CuboidSelection.h"

class CommandOrigin;
class CommandOutput;
class Player;

namespace creatorx {

// 取执行命令的玩家并检查权限；失败时已向 output 写入错误。
Player* getCommandPlayer(CommandOrigin const& origin, CommandOutput& output);

// 当前维度的完整选区；没有时已输出错误。
core::CuboidSelection* completeSelection(Player& player, Feedback const& output);

// 把选区设为指定包围盒（粘贴 -s 使用）。
void selectBox(Player& player, core::Box const& box);

// 以下操作供命令与表单共用，返回是否成功。
bool setCorner(Player& player, bool first, core::Vec3i pos, Feedback const& output);
bool setLookCorner(Player& player, bool first, Feedback const& output);
bool setSelection(Player& player, core::Vec3i first, core::Vec3i second, Feedback const& output);
bool resizeSelection(
    Player&         player,
    Feedback const& output,
    bool            contract,
    int             amount,
    int             reverseAmount,
    core::Direction direction
);
bool expandVertically(Player& player, Feedback const& output);
bool shiftSelection(Player& player, Feedback const& output, int amount, core::Direction direction);
bool outsetSelection(Player& player, Feedback const& output, int amount, bool horizontal, bool vertical);
bool insetSelection(Player& player, Feedback const& output, int amount);
void clearSelection(Player& player, Feedback const& output);
bool giveWand(Player& player, Feedback const& output);
void toggleWand(Player& player, Feedback const& output);
void toggleOutline(Player& player, Feedback const& output);
void showSize(core::CuboidSelection const& selection, Feedback const& output);

void enableSelectionCommands();
void disableSelectionCommands();

} // namespace creatorx
