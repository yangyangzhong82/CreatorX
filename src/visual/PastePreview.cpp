#include "visual/PastePreview.h"
#include "visual/SelectionOutline.h"
#include "edit/Pattern.h"
#include "mod/Global.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include <unordered_map>

namespace creatorx {
namespace {
struct Preview {
    int dimension;
    std::uint64_t token;
    core::Box box;
    std::vector<core::Vec3i> positions;
    std::chrono::steady_clock::time_point expires;
};
std::unordered_map<mce::UUID,Preview> previews;
std::uint64_t sequence=0;
ll::event::ListenerPtr tickListener, disconnectListener;
void send(Player& player,core::Vec3i p,Block const& block,unsigned layer) {
    auto raw=MinecraftPackets::createPacket(MinecraftPacketIds::UpdateBlock);
    auto& packet=static_cast<UpdateBlockPacket&>(*raw);
    packet.mPos=BlockPos{p.x,p.y,p.z};
    packet.mLayer=layer;
    packet.mRuntimeId=block.mNetworkId;
    packet.mUpdateFlags=2;
    packet.sendTo(player);
}
}
void cancelPastePreview(Player& player) {
    auto it=previews.find(player.getUuid());
    if(it==previews.end()) return;
    if(it->second.dimension==int(player.getDimensionId())) {
        auto& region=player.getDimensionBlockSource();
        for(auto p:it->second.positions) {
            send(player,p,region.getBlock({p.x,p.y,p.z}),0);
            send(player,p,region.getExtraBlock({p.x,p.y,p.z}),1);
        }
        clearSelectionOutline(player,true);
    }
    previews.erase(it);
}
std::uint64_t showPastePreview(Player& player,edit::Clipboard const& clipboard,core::Vec3i min,bool skipAir,edit::Mask const* mask) {
    cancelPastePreview(player);
    Preview preview{int(player.getDimensionId()),++sequence,{min,min+clipboard.size-core::Vec3i{1,1,1}},{},std::chrono::steady_clock::now()+std::chrono::seconds(config.visual.previewSeconds)};
    // 较大的建筑只显示落点边界，避免一次发送过多假方块。
    if(clipboard.volume()<=config.visual.previewMaxBlocks) for(int y=0;y<clipboard.size.y;++y) for(int z=0;z<clipboard.size.z;++z) for(int x=0;x<clipboard.size.x;++x) {
        auto cell=clipboard.at(clipboard.index({x,y,z}));
        if(!cell.block || (skipAir && cell.block->isAir()) || edit::isDisallowed(*cell.block)) continue;
        if(mask && !mask->test(*cell.block)) continue;
        auto p=min+core::Vec3i{x,y,z};
        preview.positions.push_back(p);
        send(player,p,*cell.block,0);
        send(player,p,cell.extra ? *cell.extra : edit::airBlock(),1);
    }
    showSelectionOutline(player,{preview.box.min,preview.box.max},true);
    auto token=preview.token;
    previews.emplace(player.getUuid(),std::move(preview));
    return token;
}
bool finishPastePreview(Player& player,std::uint64_t token) {
    auto it=previews.find(player.getUuid());
    if(it==previews.end() || it->second.token!=token) return false;
    bool valid=it->second.dimension==int(player.getDimensionId()) && std::chrono::steady_clock::now()<it->second.expires;
    cancelPastePreview(player);
    return valid;
}
void enablePreviews() {
    auto& bus=ll::event::EventBus::getInstance();
    disconnectListener=bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) { previews.erase(event.self().getUuid()); });
    tickListener=bus.emplaceListener<ll::event::ServerLevelTickEvent>([ticks=0](auto& event) mutable {
        if(++ticks<20) return; ticks=0;
        std::vector<mce::UUID> expired;
        for(auto const& [uuid,preview]:previews) {
            auto player=event.level().getPlayer(uuid);
            if(!player || preview.dimension!=int(player->getDimensionId()) || std::chrono::steady_clock::now()>=preview.expires) expired.push_back(uuid);
            else showSelectionOutline(*player,{preview.box.min,preview.box.max},true);
        }
        for(auto uuid:expired) {
            if(auto player=event.level().getPlayer(uuid)) cancelPastePreview(*player);
            else previews.erase(uuid);
        }
    });
}
void disablePreviews() {
    auto& bus=ll::event::EventBus::getInstance();
    if(tickListener) bus.removeListener(tickListener);
    if(disconnectListener) bus.removeListener(disconnectListener);
    tickListener.reset(); disconnectListener.reset();
    auto level=ll::service::getLevel();
    std::vector<mce::UUID> ids;
    for(auto const& [uuid,preview]:previews) ids.push_back(uuid);
    for(auto uuid:ids) if(level) if(auto player=level->getPlayer(uuid)) cancelPastePreview(*player);
    previews.clear();
}
}
