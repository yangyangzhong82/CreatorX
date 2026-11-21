#include "session/PlayerSession.h"

#include "exec/JobScheduler.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "mc/world/actor/player/Player.h"

#include <unordered_map>

namespace creatorx {
namespace {

std::unordered_map<mce::UUID, PlayerSession> sessions;
ll::event::ListenerPtr                       disconnectListener;

} // namespace

void rememberRecent(std::deque<std::string>& list, std::string_view text) {
    auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return;
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
    std::erase(list, text);
    list.emplace_front(text);
    if (list.size() > 10) list.pop_back();
}

PlayerSession& getSession(Player const& player) { return sessions[player.getUuid()]; }

PlayerSession* findSession(mce::UUID const& uuid) {
    auto it = sessions.find(uuid);
    return it == sessions.end() ? nullptr : &it->second;
}

bool hasSessions() { return !sessions.empty(); }

void enableSessions() {
    disconnectListener =
        ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) {
            auto uuid = event.self().getUuid();
            exec::cancelJob(uuid);
            sessions.erase(uuid);
        });
}

void disableSessions() {
    if (disconnectListener) ll::event::EventBus::getInstance().removeListener(disconnectListener);
    disconnectListener.reset();
    sessions.clear();
}

} // namespace creatorx
