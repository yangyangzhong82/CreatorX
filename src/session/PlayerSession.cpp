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

#include <algorithm>
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
    auto& session = getSession(player.getUuid());
    if (!session.brushBindingsRestored) {
        session.brushBindingsRestored = true;
        for (auto it = session.brushBindings.begin(); it != session.brushBindings.end();) {
            auto& binding = it->second;
            bool valid = true;
            if (!binding.patternText.empty()) {
                std::string error;
                binding.pattern = edit::Pattern::parse(binding.patternText, player, error);
                if (!binding.pattern) {
                    logger.warn(tr("storage.failed", error));
                    valid = false;
                }
            }
            if (valid && !binding.maskText.empty()) {
                std::string error;
                binding.mask = edit::Mask::parse(binding.maskText, player, error);
                if (!binding.mask) {
                    logger.warn(tr("storage.failed", error));
                    binding.maskText.clear();
                }
            }
            if (valid) ++it;
            else it = session.brushBindings.erase(it);
        }
    }
    return session;
}

PlayerSession& getSession(mce::UUID const& uuid) {
    auto [it, inserted] = sessions.try_emplace(uuid);
    if (inserted) {
        auto level=ll::service::getLevel();
        if(level) {
            for(unsigned char ch:level->getLevelId()) it->second.world+=fmt::format("{:02x}",ch);
        }
        if(it->second.world.empty()) it->second.world="default";
        auto path = storage::dataDir() / "players" / it->second.world / (uuid.asString() + ".nbt");
        storage::waitForWrite(path);
        if (std::filesystem::exists(path)) {
            try {
                auto tag = storage::readNbt(path);
                it->second.history.deserialize(tag["history"].get<CompoundTag>());
                it->second.wandEnabled = bool(tag["wand"]);
                if (tag.contains("remote_wand", Tag::Byte)) it->second.remoteWand = bool(tag["remote_wand"]);
                it->second.showOutline = bool(tag["outline"]);
                if (tag.contains("hud", Tag::Byte)) it->second.showHud = bool(tag["hud"]);
                if (tag.contains("hud_idle_seconds", Tag::Int)) {
                    it->second.hudIdleSeconds = std::clamp(
                        int(tag["hud_idle_seconds"]), minHudIdleSeconds, maxHudIdleSeconds
                    );
                }
                for (auto const& value : tag["patterns"].get<ListTag>()) it->second.recentPatterns.emplace_back(std::string_view(value));
                for (auto const& value : tag["masks"].get<ListTag>()) it->second.recentMasks.emplace_back(std::string_view(value));
                if (tag.contains("brush_bindings")) {
                    for (auto const& value : tag["brush_bindings"].get<ListTag>()) {
                        auto const& entry = value.get<CompoundTag>();
                        auto tool = int(entry["tool"]);
                        if (tool < 0 || tool > int(BrushTool::repl) || !entry.contains("item")) continue;
                        BrushBinding binding;
                        binding.tool = static_cast<BrushTool>(tool);
                        if (entry.contains("pattern")) binding.patternText = std::string(std::string_view(entry["pattern"]));
                        if (entry.contains("mask")) binding.maskText = std::string(std::string_view(entry["mask"]));
                        if (entry.contains("radius")) binding.radius = int(entry["radius"]);
                        if (entry.contains("height")) binding.height = int(entry["height"]);
                        if (entry.contains("iterations")) binding.iterations = int(entry["iterations"]);
                        if (entry.contains("range")) binding.range = int(entry["range"]);
                        if (entry.contains("hollow")) binding.hollow = bool(entry["hollow"]);
                        if (entry.contains("skip_air")) binding.skipAir = bool(entry["skip_air"]);
                        if (entry.contains("original")) binding.original = bool(entry["original"]);
                        it->second.brushBindings.emplace(std::string(std::string_view(entry["item"])), std::move(binding));
                    }
                }
            } catch (std::exception const& e) {
                logger.error(tr("storage.failed", e.what()));
            }
        }
        auto clipboardPath=storage::dataDir()/"players"/it->second.world/(uuid.asString()+".clipboard.nbt");
        storage::waitForWrite(clipboardPath);
        if(std::filesystem::exists(clipboardPath)) {
            try {
                auto tag=storage::readNbt(clipboardPath);
                if(bool(tag["present"])) it->second.clipboard=schematic::deserialize(tag["clipboard"].get<CompoundTag>());
            } catch(std::exception const& error) {logger.error(tr("storage.failed",error.what()));}
        }
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
        tag["remote_wand"] = session->remoteWand;
        tag["outline"] = session->showOutline;
        tag["hud"] = session->showHud;
        tag["hud_idle_seconds"] = session->hudIdleSeconds;
        auto patterns = storage::Nbt::array(), masks = storage::Nbt::array();
        for (auto const& value : session->recentPatterns) patterns.push_back(value);
        for (auto const& value : session->recentMasks) masks.push_back(value);
        tag["patterns"] = std::move(patterns);
        tag["masks"] = std::move(masks);
        auto bindings = storage::Nbt::array();
        for (auto const& [item, binding] : session->brushBindings) {
            bindings.push_back(storage::Nbt::object({
                {"item", item},
                {"tool", int(binding.tool)},
                {"pattern", binding.patternText},
                {"mask", binding.maskText},
                {"radius", binding.radius},
                {"height", binding.height},
                {"iterations", binding.iterations},
                {"range", binding.range},
                {"hollow", binding.hollow},
                {"skip_air", binding.skipAir},
                {"original", binding.original}
            }));
        }
        tag["brush_bindings"] = std::move(bindings);
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

void closeSession(mce::UUID const& uuid) {
    saveSession(uuid);
    sessions.erase(uuid);
}

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
            exec::cancelRequestedJob(uuid);
            if (!exec::hasJob(uuid)) closeSession(uuid);
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
