#include "session/PlayerSession.h"

#include "exec/JobScheduler.h"
#include "storage/Nbt.h"
#include "schematic/Schematic.h"
#include "mod/Global.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <unordered_map>
#include <unordered_set>

namespace creatorx {
namespace {

std::unordered_map<mce::UUID, PlayerSession> sessions;
ll::event::ListenerPtr                       disconnectListener;
ll::event::ListenerPtr                       spillListener;

} // namespace

void rememberRecent(std::deque<std::string>& list, std::string_view text) {
    auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return;
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
    std::erase(list, text);
    list.emplace_front(text);
    if (list.size() > 10) list.pop_back();
}

PlayerSession& getSession(Player const& player) {
    return getSession(player.getUuid());
}

PlayerSession& getSession(mce::UUID const& uuid) {
    auto [it, inserted] = sessions.try_emplace(uuid);
    if (inserted) {
        auto level=ll::service::getLevel();
        if(level) {
            for(unsigned char ch:level->getLevelId()) it->second.world+=fmt::format("{:02x}",ch);
        }
        if(it->second.world.empty()) it->second.world="default";
        storage::flushWrites();
        auto path = storage::dataDir() / "players" / it->second.world / (uuid.asString() + ".nbt");
        if (std::filesystem::exists(path)) {
            try {
                auto tag = storage::readNbt(path);
                it->second.history.deserialize(tag["history"].get<CompoundTag>());
                it->second.wandEnabled = bool(tag["wand"]);
                it->second.showOutline = bool(tag["outline"]);
                for (auto const& value : tag["patterns"].get<ListTag>()) it->second.recentPatterns.emplace_back(std::string_view(value));
                for (auto const& value : tag["masks"].get<ListTag>()) it->second.recentMasks.emplace_back(std::string_view(value));
            } catch (std::exception const& e) {
                logger.error(tr("storage.failed", e.what()));
            }
        }
    }
        auto clipboardPath=storage::dataDir()/"players"/it->second.world/(uuid.asString()+".clipboard.nbt");
        if(inserted && std::filesystem::exists(clipboardPath)) {
            try {
                auto tag=storage::readNbt(clipboardPath);
                if(bool(tag["present"])) it->second.clipboard=schematic::deserialize(tag["clipboard"].get<CompoundTag>());
            } catch(std::exception const& error) {logger.error(tr("storage.failed",error.what()));}
        }
    return it->second;
}

void saveClipboard(mce::UUID const& uuid) {
    auto session=findSession(uuid);
    if(!session) return;
    auto path = storage::dataDir() / "players" / session->world / (uuid.asString() + ".clipboard.nbt");
    storage::queueNbt(std::move(path), [clipboard = session->clipboard] {
        CompoundTag tag;
        tag["present"] = bool(clipboard);
        if (clipboard) tag["clipboard"] = schematic::serialize(*clipboard);
        return tag;
    });
}

void saveSession(mce::UUID const& uuid) {
    auto* session = findSession(uuid);
    if (!session) return;
    try {
        CompoundTag tag;
        tag["history"] = session->history.serialize();
        tag["wand"] = session->wandEnabled;
        tag["outline"] = session->showOutline;
        auto patterns = storage::Nbt::array(), masks = storage::Nbt::array();
        for (auto const& value : session->recentPatterns) patterns.push_back(value);
        for (auto const& value : session->recentMasks) masks.push_back(value);
        tag["patterns"] = std::move(patterns);
        tag["masks"] = std::move(masks);
        storage::queueNbt(storage::dataDir() / "players" / session->world / (uuid.asString() + ".nbt"), std::move(tag),session->history.pendingWrites());
    } catch (std::exception const& e) {
        logger.error(tr("storage.failed", e.what()));
    }
}

PlayerSession* findSession(mce::UUID const& uuid) {
    auto it = sessions.find(uuid);
    return it == sessions.end() ? nullptr : &it->second;
}

bool hasSessions() { return !sessions.empty(); }

void enableSessions() {
    storage::startStorage();
    spillListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
        for (auto& [uuid, session] : sessions) session.history.pollSpills();
    });
    // 清理已不再被任何玩家历史引用的缓存，保留离线玩家的有效记录。
    try {
        std::unordered_set<std::string> retained;
        auto players=storage::dataDir()/"players";
        if(std::filesystem::exists(players)) for(auto const& file:std::filesystem::recursive_directory_iterator(players)) {
            if(!file.is_regular_file() || file.path().extension()!=".nbt" || file.path().filename().string().ends_with(".clipboard.nbt")) continue;
            auto tag=storage::readNbt(file.path());
            if(!tag.contains("history")) continue;
            for(std::string_view key:{"undo","redo"}) for(auto const& entry:tag["history"][key].get<ListTag>()) {
                retained.emplace(std::string_view(entry["file"]));
            }
        }
        auto cache=storage::dataDir()/"history-cache";
        if(std::filesystem::exists(cache)) for(auto const& file:std::filesystem::directory_iterator(cache)) {
            if(file.is_regular_file() && file.path().extension()==".nbt" && !retained.contains(file.path().filename().string())) std::filesystem::remove(file.path());
        }
    } catch(std::exception const& error) {logger.warn(tr("storage.failed",error.what()));}
    disconnectListener =
        ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) {
            auto uuid = event.self().getUuid();
            exec::cancelJob(uuid);
            saveSession(uuid);
            sessions.erase(uuid);
        });
}

void disableSessions() {
    if (spillListener) ll::event::EventBus::getInstance().removeListener(spillListener);
    spillListener.reset();
    if (disconnectListener) ll::event::EventBus::getInstance().removeListener(disconnectListener);
    disconnectListener.reset();
    for (auto const& [uuid, session] : sessions) saveSession(uuid);
    sessions.clear();
}

} // namespace creatorx
