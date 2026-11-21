#pragma once

#include "command/Feedback.h"
#include "core/region/Box.h"

#include <optional>
#include <string_view>

class Player;

namespace creatorx {

struct PasteOptions {
    bool skipAir  = false; // -a
    bool original = false; // -o
    bool select   = false; // -s
};

// 以下操作供命令与表单共用。返回 true 表示任务已提交或已弹出确认窗口。
bool regionEdit(
    Player&                         player,
    Feedback const&                 output,
    core::BoxShape                  shape,
    std::string_view                pattern,
    std::optional<std::string_view> mask
);
bool replaceBlocks(Player& player, Feedback const& output, std::string_view text);
bool copySelection(Player& player, Feedback const& output, bool cut, std::optional<std::string_view> pattern);
bool pasteClipboard(Player& player, Feedback const& output, PasteOptions options);
bool runHistory(Player& player, Feedback const& output, bool undo, int times);
void cancelPlayerJob(Player& player, Feedback const& output);
void clearClipboard(Player& player, Feedback const& output);
void clearHistory(Player& player, Feedback const& output);

void enableEditCommands();
void disableEditCommands();

} // namespace creatorx
