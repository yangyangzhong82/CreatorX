#pragma once

#include "core/region/CuboidSelection.h"
#include "edit/ChangeSet.h"
#include "edit/Clipboard.h"

#include "mc/platform/UUID.h"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class Player;

namespace creatorx {

struct WandClick {
    core::Vec3i                           position;
    int                                   dimension;
    std::chrono::steady_clock::time_point time;
};

struct PlayerSession {
    int                                    dimension{};
    core::CuboidSelection                  cuboid;
    bool                                   wandEnabled = true;
    bool                                   showOutline = true;
    std::optional<WandClick>               lastLeftClick;
    std::chrono::steady_clock::time_point  lastMenuOpen{};
    std::shared_ptr<edit::Clipboard const> clipboard;
    edit::History                          history;
    std::deque<std::string>                recentPatterns; // 最近使用的图案，最新的在前
    std::deque<std::string>                recentMasks;
};

// 记入最近使用列表：去重、置顶，最多保留 10 条。
void rememberRecent(std::deque<std::string>& list, std::string_view text);

PlayerSession& getSession(Player const& player);
PlayerSession* findSession(mce::UUID const& uuid);
bool           hasSessions();

// 玩家退出时先结束其任务，再丢弃会话。
void enableSessions();
void disableSessions();

} // namespace creatorx
