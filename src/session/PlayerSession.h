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

inline constexpr int minHudIdleSeconds = 5;
inline constexpr int maxHudIdleSeconds = 60;
inline constexpr int defaultHudIdleSeconds = 15;

struct WandClick {
    core::Vec3i                           position;
    int                                   dimension;
    std::chrono::steady_clock::time_point time;
};

struct PlayerSession {
    std::string world;
    int                                    dimension{};
    core::CuboidSelection                  cuboid;
    bool                                   wandEnabled = true;
    bool                                   remoteWand = false;
    bool                                   showOutline = true;
    bool                                   showHud = true;
    int                                    hudIdleSeconds = defaultHudIdleSeconds;
    std::chrono::steady_clock::time_point  lastHudActivity{};
    std::optional<bool>                    hudVisible;
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
PlayerSession& getSession(mce::UUID const& uuid);
PlayerSession* findSession(mce::UUID const& uuid);
bool           hasSessions();
void saveSession(mce::UUID const& uuid);
void saveClipboard(mce::UUID const& uuid);

// 玩家退出时先结束其任务，再丢弃会话。
void enableSessions();
void disableSessions();

} // namespace creatorx
