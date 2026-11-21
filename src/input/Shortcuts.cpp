#include "input/Shortcuts.h"

#ifdef LL_PLAT_C
#include "ll/api/input/KeyRegistry.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/PacketSender.h"
#include "mc/network/packet/CommandRequestPacket.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/PlayerCommandOrigin.h"

#include <Windows.h>
#include <memory>

namespace creatorx {
namespace {

struct Shortcut {
    char const* name;
    int         key;
    char const* command;
};

constexpr Shortcut shortcuts[]{
    {"hpos1", VK_F6, "/cx hpos1"},
    {"hpos2", VK_F7, "/cx hpos2"},
    {"undo", VK_F8, "/cx undo"},
    {"redo", VK_F9, "/cx redo"},
    {"menu", VK_F10, "/cx menu"},
};

} // namespace

void registerShortcuts() {
    auto& registry = ll::input::KeyRegistry::getInstance();
    for (auto const& shortcut : shortcuts) {
        registry.getOrCreateKey(shortcut.name, {shortcut.key}).registerButtonDownHandler(
            [command = shortcut.command](FocusImpact, IClientInstance& client) {
                auto* player = client.getLocalPlayer();
                if (!player || !client.isInGameInputEnabled()) return;

                auto origin = std::make_unique<PlayerCommandOrigin>(player->getLevel(), player->getOrCreateUniqueID());
                CommandContext context(command, std::move(origin), static_cast<int>(CurrentCmdVersion::Latest));
                CommandRequestPacket packet{CommandRequestPacketPayload{context, false}};
                client.getPacketSender().sendToServer(packet);
            }
        );
    }
}

} // namespace creatorx
#endif
