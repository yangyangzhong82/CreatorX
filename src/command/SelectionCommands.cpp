#include "command/SelectionCommands.h"

#include "core/region/CuboidSelection.h"
#include "mod/Global.h"
#include "visual/SelectionOutline.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ExecuteCommandEvent.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/platform/UUID.h"
#include "mc/safety/RedactableString.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace creatorx {

struct PositionParams {
    ll::command::Optional<CommandPosition> position;
};

struct ResizeParams {
    int                                    amount;
    ll::command::Optional<int>             reverseAmount;
    ll::command::Optional<core::Direction> direction;
};

enum class VerticalExpansion { vert };

struct VerticalParams {
    VerticalExpansion direction;
};

namespace {

struct WandClick {
    core::Vec3i                           position;
    int                                   dimension;
    std::chrono::steady_clock::time_point time;
};

struct PlayerSelection {
    int                      dimension{};
    core::CuboidSelection    cuboid;
    bool                     wandEnabled = true;
    std::optional<WandClick> lastLeftClick;
};

constexpr std::array<std::string_view, 10>
    commandNames{"pos1", "pos2", "hpos1", "hpos2", "size", "desel", "wand", "togglewand", "expand", "contract"};
constexpr auto                                 wandTag = "creatorx:selection_wand";
std::unordered_map<mce::UUID, PlayerSelection> selections;
ll::event::ListenerPtr                         disconnectListener;
ll::event::ListenerPtr                         commandListener;
ll::event::ListenerPtr                         registerListener;
ll::event::ListenerPtr                         destroyListener;
ll::event::ListenerPtr                         interactListener;
ll::event::ListenerPtr                         tickListener;

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

struct SelectionResult {
    bool        success;
    std::string message;

    void report(CommandOutput& output) const {
        if (success) output.success(message);
        else output.error(message);
    }
};

SelectionResult setCorner(Player& player, bool first, BlockPos const& pos) {
    int   dimension = player.getDimensionId();
    auto& session   = selections[player.getUuid()];
    auto  candidate = session.dimension == dimension ? session.cuboid : core::CuboidSelection{};
    (first ? candidate.first : candidate.second) = core::Vec3i{pos.x, pos.y, pos.z};
    auto size                                    = candidate.size();
    if (size && !size->fits(config.limits.maxSelectionVolume)) {
        return {false, tr("selection.too_large", config.limits.maxSelectionVolume)};
    }
    session.dimension = dimension;
    session.cuboid    = candidate;
    if (size) showSelectionOutline(player, candidate);
    return {true, tr("selection.corner", first ? 1 : 2, pos.x, pos.y, pos.z)};
}

core::CuboidSelection* completeSelection(Player& player, CommandOutput& output) {
    auto it = selections.find(player.getUuid());
    if (it == selections.end() || it->second.dimension != int(player.getDimensionId())
        || (!it->second.cuboid.first && !it->second.cuboid.second)) {
        output.error(tr("selection.none_here"));
        return nullptr;
    }
    auto& selection = it->second.cuboid;
    if (!selection.first || !selection.second) {
        output.error(tr("selection.missing_corner", selection.first ? 2 : 1));
        return nullptr;
    }
    return &selection;
}

void showSize(core::CuboidSelection const& selection, CommandOutput& output) {
    auto size = *selection.size();
    auto a    = *selection.first;
    auto b    = *selection.second;
    output.success(tr("selection.size", a.x, a.y, a.z, b.x, b.y, b.z, size.x, size.y, size.z, size.volume()));
}

core::Direction facingDirection(Player const& player) {
    using core::Direction;
    auto view = player.getViewVector();
    if (std::abs(view.y) >= std::max(std::abs(view.x), std::abs(view.z))) {
        return view.y >= 0 ? Direction::up : Direction::down;
    }
    if (std::abs(view.x) >= std::abs(view.z)) {
        return view.x >= 0 ? Direction::east : Direction::west;
    }
    return view.z >= 0 ? Direction::south : Direction::north;
}

void commitResize(
    Player&                      player,
    CommandOutput&               output,
    core::CuboidSelection&       selection,
    core::CuboidSelection const& candidate
) {
    if (!candidate.size()->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return;
    }
    selection = candidate;
    showSelectionOutline(player, selection);
    showSize(selection, output);
}

void resizeSelection(Player& player, CommandOutput& output, bool contract, ResizeParams const& args) {
    auto* selection = completeSelection(player, output);
    if (!selection) return;

    int reverse = args.reverseAmount.value_or(0);
    if (args.amount < 0 || reverse < 0 || (args.amount == 0 && reverse == 0)) {
        output.error(tr("selection.resize_amount"));
        return;
    }
    auto direction = args.direction.value_or(core::Direction::me);
    if (direction == core::Direction::me) direction = facingDirection(player);
    auto candidate = *selection;
    if (!candidate.resize(direction, args.amount, reverse, contract)) {
        output.error(tr(contract ? "selection.contract_empty" : "selection.expand_range"));
        return;
    }
    commitResize(player, output, *selection, candidate);
}

void expandVertically(Player& player, CommandOutput& output) {
    auto* selection = completeSelection(player, output);
    if (!selection) return;
    auto const& range     = *player.getDimension().mHeightRange;
    auto        candidate = *selection;
    candidate.expandVertical(range.mMin, int(range.mMax) - 1);
    commitResize(player, output, *selection, candidate);
}

void setLookCorner(Player& player, bool first, CommandOutput& output) {
    auto hit = player.traceRay(config.selection.traceDistance, false, true);
    if (hit.mType != HitResultType::Tile) {
        output.error(tr("selection.no_target", config.selection.traceDistance));
        return;
    }
    setCorner(player, first, hit.mBlock).report(output);
}

void giveWand(Player& player, CommandOutput& output) {
    ItemStack item;
    item.reinit(config.wand.item, 1, 0);
    if (item.isNull()) {
        output.error(tr("wand.invalid_item", config.wand.item));
        return;
    }
    auto tag = item.mUserData ? std::make_unique<CompoundTag>(*item.mUserData) : std::make_unique<CompoundTag>();
    tag->putBoolean(wandTag, true);
    item.setUserData(std::move(tag));
    item.setCustomName(Bedrock::Safety::RedactableString(std::string(tr("wand.name"))));
    if (!player.add(item)) {
        output.error(tr("wand.inventory_full"));
        return;
    }
    player.sendInventory(false);
    output.success(tr("wand.given"));
}

bool tryWandClick(Player& player, BlockPos const& pos, bool first, ItemStack const& item) {
    if (player.isClientSide() || player.getLevel().isClientSide()
        || player.getCommandPermissionLevel() < CommandPermissionLevel::GameDirectors || item.isNull()
        || item.getTypeName() != config.wand.item
        || (config.wand.requireTag && (!item.mUserData || !item.mUserData->contains(wandTag)))) {
        return false;
    }
    auto& session = selections[player.getUuid()];
    if (!session.wandEnabled) return false;
    if (first) {
        auto        now = std::chrono::steady_clock::now();
        core::Vec3i position{pos.x, pos.y, pos.z};
        int         dimension = player.getDimensionId();
        auto const& last      = session.lastLeftClick;
        if (last && last->position == position && last->dimension == dimension
            && now - last->time < std::chrono::milliseconds(200)) {
            return true;
        }
        session.lastLeftClick = WandClick{position, dimension, now};
    }
    player.sendMessage(setCorner(player, first, pos).message);
    return true;
}

LL_TYPE_INSTANCE_HOOK(
    WandStartDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$startDestroyBlock,
    bool,
    BlockPos const& position,
    uchar           face,
    bool&           hasDestroyedBlock
) {
    Player& player = mPlayer;
    if (tryWandClick(player, position, true, player.getSelectedItem())) {
        hasDestroyedBlock = false;
        return false;
    }
    return origin(position, face, hasDestroyedBlock);
}

std::optional<ll::memory::HookRegistrar<WandStartDestroyHook>> wandStartDestroy;

void registerResizeCommand(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    bool contract = name == "contract";
    auto execute  = [contract](CommandOrigin const& origin, CommandOutput& output, ResizeParams const& args) {
        if (auto* player = getPlayer(origin, output)) resizeSelection(*player, output, contract, args);
    };

    auto directional = handle.overload<ResizeParams>();
    if (subcommand) (void)directional.text(name);
    directional.required("amount").required("direction").execute(execute);

    auto resize = handle.overload<ResizeParams>();
    if (subcommand) (void)resize.text(name);
    resize.required("amount").optional("reverseAmount").optional("direction").execute(execute);

    if (!contract) {
        auto vertical = handle.overload<VerticalParams>();
        if (subcommand) (void)vertical.text(name);
        vertical.required("direction").execute([](CommandOrigin const& origin, CommandOutput& output) {
            if (auto* player = getPlayer(origin, output)) expandVertically(*player, output);
        });
    }
}

void registerSelectionCommand(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    if (name == "expand" || name == "contract") {
        registerResizeCommand(handle, name, subcommand);
        return;
    }
    if (name == "pos1" || name == "pos2") {
        auto overload = handle.overload<PositionParams>();
        if (subcommand) (void)overload.text(name);
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
                setCorner(*player, first, pos).report(output);
            });
        return;
    }
    auto overload = handle.overload();
    if (subcommand) (void)overload.text(name);
    overload.execute([name](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayer(origin, output);
        if (!player) return;
        if (name == "size") {
            if (auto* selection = completeSelection(*player, output)) showSize(*selection, output);
        } else if (name == "desel") {
            auto it = selections.find(player->getUuid());
            if (it != selections.end()) {
                it->second.cuboid = {};
                it->second.lastLeftClick.reset();
            }
            output.success(tr("selection.cleared"));
        } else if (name == "wand") {
            giveWand(*player, output);
        } else if (name == "togglewand") {
            auto& enabled = selections[player->getUuid()].wandEnabled;
            enabled       = !enabled;
            output.success(tr(enabled ? "wand.enabled" : "wand.disabled"));
        } else {
            setLookCorner(*player, name == "hpos1", output);
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
    destroyListener    = bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>([](auto& event) {
        auto& player = event.self();
        if (!event.isCancelled() && tryWandClick(player, event.pos(), true, player.getSelectedItem())) event.cancel();
    });
    interactListener   = bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([](auto& event) {
        if (!event.isCancelled() && tryWandClick(event.self(), event.blockPos(), false, event.item())) event.cancel();
    });
    wandStartDestroy.emplace();
    if (config.visual.mode != "none") {
        tickListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>([ticks = 0](auto& event) mutable {
            if (++ticks < 30) return;
            ticks = 0;
            if (selections.empty()) return;
            event.level().forEachPlayer(std::function<bool(Player&)>([](Player& player) {
                auto it = selections.find(player.getUuid());
                if (it != selections.end() && it->second.dimension == int(player.getDimensionId())) {
                    showSelectionOutline(player, it->second.cuboid);
                }
                return true;
            }));
        });
    }
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
    if (destroyListener) bus.removeListener(destroyListener);
    if (interactListener) bus.removeListener(interactListener);
    if (tickListener) bus.removeListener(tickListener);
    wandStartDestroy.reset();
    disconnectListener.reset();
    commandListener.reset();
    registerListener.reset();
    destroyListener.reset();
    interactListener.reset();
    tickListener.reset();
    selections.clear();
}

} // namespace creatorx
