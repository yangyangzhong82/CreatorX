#include "command/SelectionCommands.h"

#include "core/region/CuboidSelection.h"
#include "mod/Global.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ExecuteCommandEvent.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/platform/UUID.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/level/dimension/DimensionType.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>

namespace creatorx {

struct PositionParams {
    ll::command::Optional<CommandPosition> position;
};

namespace {

struct PlayerSelection {
    int                   dimension;
    core::CuboidSelection cuboid;
};

constexpr std::array<std::string_view, 4>      commandNames{"pos1", "pos2", "size", "desel"};
std::unordered_map<mce::UUID, PlayerSelection> selections;
ll::event::ListenerPtr                         disconnectListener;
ll::event::ListenerPtr                         commandListener;
ll::event::ListenerPtr                         registerListener;

Player* getPlayer(CommandOrigin const& origin, CommandOutput& output) {
    auto* actor = origin.getEntity();
    if (!actor || !actor->isPlayer()) {
        output.error(tr("command.player_only"));
        return nullptr;
    }
    if (origin.getPermissionsLevel() < CommandPermissionLevel::GameDirectors) {
        output.error(tr("command.no_permission"));
        return nullptr;
    }
    return static_cast<Player*>(actor);
}

void setCorner(Player& player, bool first, BlockPos const& pos, CommandOutput& output) {
    int dimension       = player.getDimensionId();
    auto [it, inserted] = selections.try_emplace(player.getUuid(), PlayerSelection{dimension, {}});
    auto& session       = it->second;
    if (!inserted && session.dimension != dimension) {
        session = {dimension, {}};
    }
    auto candidate                               = session.cuboid;
    (first ? candidate.first : candidate.second) = core::Vec3i{pos.x, pos.y, pos.z};
    auto size                                    = candidate.size();
    if (size && !size->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return;
    }
    session.cuboid = candidate;
    output.success(tr("selection.corner", first ? 1 : 2, pos.x, pos.y, pos.z));
}

void showSize(Player& player, CommandOutput& output) {
    auto it = selections.find(player.getUuid());
    if (it == selections.end() || it->second.dimension != int(player.getDimensionId())) {
        output.error(tr("selection.none_here"));
        return;
    }
    auto const& selection = it->second.cuboid;
    auto        size      = selection.size();
    if (!size) {
        output.error(tr("selection.missing_corner", selection.first ? 2 : 1));
        return;
    }
    auto a = *selection.first;
    auto b = *selection.second;
    output.success(tr("selection.size", a.x, a.y, a.z, b.x, b.y, b.z, size->x, size->y, size->z, size->volume()));
}

void registerSelectionCommand(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    if (name == "pos1" || name == "pos2") {
        auto overload = handle.overload<PositionParams>();
        if (subcommand) overload.text(name);
        overload.optional("position")
            .execute([first = name == "pos1"](
                         CommandOrigin const&  origin,
                         CommandOutput&        output,
                         PositionParams const& args,
                         Command const&        command
                     ) {
                auto* player = getPlayer(origin, output);
                if (!player) return;
                auto pos = args.position ? args.position->getBlockPos(command.mVersion, origin, Vec3{0, 0, 0})
                                         : player->getFeetBlockPos();
                setCorner(*player, first, pos, output);
            });
        return;
    }
    auto overload = handle.overload();
    if (subcommand) overload.text(name);
    overload.execute([name](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayer(origin, output);
        if (!player) return;
        if (name == "size") {
            showSize(*player, output);
        } else {
            selections.erase(player->getUuid());
            output.success(tr("selection.cleared"));
        }
    });
}

void registerCommands() {
    auto& registrar   = ll::command::CommandRegistrar::getServerInstance();
    auto  description = std::string(tr("command.selection"));
    auto& cx          = registrar.getOrCreateCommand("cx", description, CommandPermissionLevel::GameDirectors);
    for (auto name : commandNames) {
        registerSelectionCommand(cx, name, true);
        if (config.command.shortCommands) {
            auto& handle =
                registrar.getOrCreateCommand(std::string(name), description, CommandPermissionLevel::GameDirectors);
            registerSelectionCommand(handle, name, false);
        }
    }
}

} // namespace

void enableSelectionCommands() {
    auto& bus        = ll::event::EventBus::getInstance();
    registerListener = bus.emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto&) { registerCommands(); },
        ll::event::EventPriority::Low
    );
    if (ll::service::getCommandRegistry(false)) registerCommands();
    disconnectListener = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) {
        selections.erase(event.self().getUuid());
    });
    if (config.command.chatDoubleSlash) {
        commandListener = bus.emplaceListener<ll::event::ExecutingCommandEvent>([](auto& event) {
            auto& command = event.commandContext().mCommand;
            if (!command.starts_with("//")) return;
            auto rest = std::string_view(command).substr(2);
            auto name = rest.substr(0, rest.find_first_of(" \t"));
            if (std::ranges::find(commandNames, name) != commandNames.end()) {
                command.replace(0, 2, "/cx ");
            }
        });
    }
}

void disableSelectionCommands() {
    auto& bus = ll::event::EventBus::getInstance();
    if (registerListener) bus.removeListener(registerListener);
    if (disconnectListener) bus.removeListener(disconnectListener);
    if (commandListener) bus.removeListener(commandListener);
    disconnectListener.reset();
    commandListener.reset();
    registerListener.reset();
    selections.clear();
}

} // namespace creatorx
