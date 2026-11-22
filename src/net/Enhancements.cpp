#include "net/Enhancements.h"
#include "mod/Global.h"
#include "session/PlayerSession.h"
#include "exec/JobScheduler.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#ifdef LL_PLAT_C
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/gui/GuiData.h"
#endif
#include <mutex>
#include <atomic>
#include <algorithm>
#include <chrono>

namespace creatorx {
namespace {
using namespace ll::network;
using Clock = std::chrono::steady_clock;
class StatusPacket final : public ll::network::Packet {
public:
    static constexpr std::string_view CustomPacketId = "creatorx:status";
    static constexpr uint PacketVersion = 1;
    static constexpr bool Optional = true;
    static constexpr PacketSide Side = PacketSide::Client;
    std::string text;
    std::string_view getName() const override { return CustomPacketId; }
    PacketRuntimeId getRuntimeId() const override { return getPacketRuntimeId<StatusPacket>; }
    static bool canSendTo(Player const& player) { return ll::network::Packet::canSendTo(getPacketRuntimeId<StatusPacket>,player); }
    void write(BinaryStream& stream) const override { stream.writeString(text,nullptr,nullptr); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override { return stream.getString(text,16384); }
};
ll::event::ListenerPtr serverTick;
#ifdef LL_PLAT_C
ll::event::ListenerPtr clientTick, exitListener;
std::atomic_bool hudEnabled=true,localCoordinates=true;
std::mutex hudMutex;
std::string hud;
std::string displayedHud;
Clock::time_point lastStatus;
Clock::time_point lastLocalActivity;

void clearDisplayedHud(GuiData& gui) {
    if (!displayedHud.empty() && *gui.mTitleMessage->mActionBarMessage == displayedHud) {
        gui.setActionBarMessage({}, std::nullopt);
    }
    displayedHud.clear();
}

void resetHud() {
    std::lock_guard lock(hudMutex);
    hud.clear();
    displayedHud.clear();
    lastStatus = {};
    lastLocalActivity = {};
}

class StatusHandler final : public IPacketHandler {
public:
    void handle(NetworkIdentifier const&, NetEventCallback&, PacketSide, ll::network::Packet const& raw) const override {
        std::lock_guard lock(hudMutex);
        hud=static_cast<StatusPacket const&>(raw).text;
        lastStatus=std::chrono::steady_clock::now();
    }
} handler;
#endif
}
bool hasEnhancedClient(Player const& player) { return StatusPacket::canSendTo(player); }

void noteHudActivity(Player const& player) {
    getSession(player).lastHudActivity = Clock::now();
}

void setHudPreferences(Player const& player, bool show, int idleSeconds) {
    auto& session = getSession(player);
    session.showHud = show;
    session.hudIdleSeconds = std::clamp(idleSeconds, minHudIdleSeconds, maxHudIdleSeconds);
    session.lastHudActivity = Clock::now();
    if (!show && hasEnhancedClient(player)) {
        StatusPacket{}.sendTo(player);
        session.hudVisible = false;
    }
    saveSession(player.getUuid());
}

#ifdef LL_PLAT_C
void noteLocalHudActivity() {
    std::lock_guard lock(hudMutex);
    lastLocalActivity = Clock::now();
}
#endif

void registerEnhancementProtocol() {
    auto& registry=ll::network::PacketRegistrar::getInstance();
    registry.registerPacket("creatorx:status",getPacketRuntimeId<StatusPacket>,[] { return std::make_unique<StatusPacket>(); },StatusPacket::Side,StatusPacket::Optional);
#ifdef LL_PLAT_C
    registry.registerHandler("creatorx:status",getPacketRuntimeId<StatusPacket>,handler,PacketSide::Client);
#endif
}
void refreshEnhancementOptions() {
#ifdef LL_PLAT_C
    hudEnabled=config.client.hud;
    localCoordinates=config.client.localCoordinates;
#endif
}
void enableEnhancements() {
    refreshEnhancementOptions();
    serverTick=ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([ticks=0](auto& event) mutable {
        if (++ticks<20) return;
        ticks=0;
        event.level().forEachPlayer(std::function<bool(Player&)>([](Player& player) {
            if (!hasEnhancedClient(player)) return true;
            auto session=findSession(player.getUuid());
            if (!session) return true;
            auto now = Clock::now();
            auto progress = exec::jobPercent(player.getUuid());
            if (progress) session->lastHudActivity = now;
            bool visible = session->showHud
                && session->lastHudActivity != Clock::time_point{}
                && now - session->lastHudActivity < std::chrono::seconds(session->hudIdleSeconds);
            if (!visible) {
                if (session->hudVisible != false) {
                    StatusPacket{}.sendTo(player);
                    session->hudVisible = false;
                }
                return true;
            }

            StatusPacket packet;
            packet.text="CreatorX";
            if (session->dimension==int(player.getDimensionId())) if(auto size=session->cuboid.size()) packet.text+=tr("hud.selection",size->x,size->y,size->z);
            if(session->clipboard) {auto size=session->clipboard->size; packet.text+=tr("hud.clipboard",size.x,size.y,size.z);}
            if(progress) packet.text+=tr("hud.progress",*progress);
            else packet.text+=tr("hud.history",session->history.undoCount(),session->history.redoCount());
            packet.sendTo(player);
            session->hudVisible = true;
            return true;
        }));
    });
#ifdef LL_PLAT_C
    clientTick=ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>([ticks=0](auto&) mutable {
        if(++ticks<10) return;
        ticks=0;
        auto client=ll::service::getClientInstance();
        if(!client) return;
        auto player=client->getLocalPlayer();
        if(!player) return;
        auto& gui = *client->getGuiData();
        auto now = Clock::now();
        std::string text;
        bool showLocal = false;
        std::lock_guard lock(hudMutex);
        if (hudEnabled && client->isInGameInputEnabled()) {
            if (lastStatus == Clock::time_point{}) {
                showLocal = localCoordinates
                    && lastLocalActivity != Clock::time_point{}
                    && now - lastLocalActivity < std::chrono::seconds(defaultHudIdleSeconds);
            } else if (now - lastStatus < std::chrono::seconds(4)) {
                text = hud;
            }
        }
        if (showLocal) {
            auto pos = player->getFeetBlockPos();
            text = tr("hud.local", pos.x, pos.y, pos.z);
        }
        if (text.empty()) {
            clearDisplayedHud(gui);
        } else {
            gui.setActionBarMessage(text, std::nullopt);
            displayedHud = std::move(text);
        }
    });
    exitListener=ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
        resetHud();
    });
#endif
}
void disableEnhancements() {
    auto& bus=ll::event::EventBus::getInstance();
    if(serverTick) bus.removeListener(serverTick);
    serverTick.reset();
#ifdef LL_PLAT_C
    if(clientTick) bus.removeListener(clientTick);
    if(exitListener) bus.removeListener(exitListener);
    clientTick.reset(); exitListener.reset();
    resetHud();
#endif
}
void unregisterEnhancementProtocol() {
    ll::network::PacketRegistrar::getInstance().unregisterPacket(getPacketRuntimeId<StatusPacket>);
}
}
