#pragma once

#include "command/Feedback.h"
#include "core/region/Box.h"
#include "core/region/CuboidSelection.h"

#include <optional>
#include <string_view>
#include <memory>
#include <functional>

class Player;
namespace creatorx::exec { class Job; }

namespace creatorx {

struct PasteOptions {
    bool skipAir  = false; // -a
    bool original = false; // -o
    bool select   = false; // -s
    bool preview  = false; // -p
    bool entities = false; // -e
    std::optional<std::string> mask;
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
bool copySelection(Player& player, Feedback const& output, bool cut, std::optional<std::string_view> pattern, std::optional<std::string_view> mask = {}, bool includeEntities = false);
bool pasteClipboard(Player& player, Feedback const& output, PasteOptions options);
bool runHistory(Player& player, Feedback const& output, bool undo, int times, std::string_view target = {});
void cancelPlayerJob(Player& player, Feedback const& output);
void clearClipboard(Player& player, Feedback const& output);
void clearHistory(Player& player, Feedback const& output);
bool reloadConfiguration(Feedback const& output);
bool rotateClipboard(Player& player, Feedback const& output, int y, int x = 0, int z = 0);
bool flipClipboard(Player& player, Feedback const& output, core::Direction direction);

void enableEditCommands();
void disableEditCommands();
bool submitEditJob(Player& player, Feedback const& output, std::unique_ptr<exec::Job> job, std::uint64_t changes, std::function<void(Player&)> onSubmitted = {});

} // namespace creatorx
