#include "command/SelectionCommands.h"

#include "core/region/CuboidSelection.h"
#include "mod/Global.h"
#include "net/Enhancements.h"
#include "session/PlayerSession.h"
#include "ui/Menu.h"
#include "visual/SelectionOutline.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/player/PlayerUseItemEvent.h"
#include "ll/api/event/player/PlayerSwingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/platform/UUID.h"
#include "mc/safety/RedactableString.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/server/commands/CommandRawText.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string_view>

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

struct ShiftParams {
    int                             amount;
    ll::command::Optional<core::Direction> direction;
};

struct AmountParams {
    int amount;
    ll::command::Optional<CommandRawText> flags;
};

namespace {

constexpr std::array<std::string_view, 13> commandNames{
    "pos1", "pos2", "hpos1", "hpos2", "size", "desel", "wand", "togglewand", "expand", "contract", "shift", "outset", "inset"
};
constexpr auto         wandTag = "creatorx:selection_wand";
ll::event::ListenerPtr registerListener;
ll::event::ListenerPtr destroyListener;
ll::event::ListenerPtr interactListener;
ll::event::ListenerPtr useItemListener;
ll::event::ListenerPtr swingListener;
ll::event::ListenerPtr tickListener;

void refreshOutline(Player& player, PlayerSession const& session) {
    if (session.showOutline) showSelectionOutline(player, session.cuboid);
}

} // namespace

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

namespace {

bool commitResize(
    Player&                      player,
    Feedback const&              output,
    core::CuboidSelection&       selection,
    core::CuboidSelection const& candidate
) {
    if (!candidate.size()->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return false;
    }
    selection = candidate;
    refreshOutline(player, getSession(player));
    showSize(selection, output);
    return true;
}

bool isWand(Player& player, ItemStack const& item) {
    return !player.isClientSide() && !player.getLevel().isClientSide()
        && player.getCommandPermissionLevel() >= CommandPermissionLevel::GameDirectors && !item.isNull()
        && item.getTypeName() == config.wand.item
        && (!config.wand.requireTag || (item.mUserData && item.mUserData->contains(wandTag)));
}

// 蹲下右键魔杖打开主菜单；按住右键会连续触发，用会话里的时间去抖。
bool tryOpenMenu(Player& player, PlayerSession& session) {
    if (!config.wand.sneakOpensMenu || !player.isSneaking()) return false;
    auto now = std::chrono::steady_clock::now();
    if (now - session.lastMenuOpen >= std::chrono::milliseconds(500)) {
        session.lastMenuOpen = now;
        ui::openMainMenu(player);
    }
    return true;
}

bool tryWandClick(Player& player, BlockPos const& pos, bool first, ItemStack const& item) {
    if (!isWand(player, item)) return false;
    auto& session = getSession(player);
    if (!session.wandEnabled) return false;
    if (!first && tryOpenMenu(player, session)) return true;

    core::Vec3i position{pos.x, pos.y, pos.z};
    int         dimension = player.getDimensionId();
    if (!first && session.dimension == dimension && session.cuboid.second == position) return true;

    if (first) {
        auto        now  = std::chrono::steady_clock::now();
        auto const& last = session.lastLeftClick;
        if (last && last->position == position && last->dimension == dimension
            && now - last->time < std::chrono::milliseconds(200)) {
            return true;
        }
        session.lastLeftClick = WandClick{position, dimension, now};
    }
    setCorner(player, first, position, Feedback(player));
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
        auto* player = getCommandPlayer(origin, output);
        if (!player) return;
        resizeSelection(
            *player,
            output,
            contract,
            args.amount,
            args.reverseAmount.value_or(0),
            args.direction.value_or(core::Direction::me)
        );
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
            if (auto* player = getCommandPlayer(origin, output)) expandVertically(*player, output);
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
                auto* player = getCommandPlayer(origin, output);
                if (!player) return;
                auto pos = args.position ? args.position->getBlockPos(command.mVersion, origin, Vec3{0, 0, 0})
                                         : player->getFeetBlockPos();
                setCorner(*player, first, {pos.x, pos.y, pos.z}, output);
            });
        return;
    }
    if (name == "shift") {
        auto overload = handle.overload<ShiftParams>();
        if (subcommand) (void)overload.text(name);
        overload.required("amount").optional("direction").execute([](CommandOrigin const& origin, CommandOutput& output, ShiftParams const& args) {
            if (auto* player = getCommandPlayer(origin, output)) {
                auto direction = args.direction.value_or(core::Direction::me);
                if (direction == core::Direction::me) direction = facingDirection(*player);
                shiftSelection(*player, output, args.amount, direction);
            }
        });
        return;
    }
    if (name == "outset" || name == "inset") {
        auto overload = handle.overload<AmountParams>();
        if (subcommand) (void)overload.text(name);
        overload.required("amount").optional("flags").execute([name](CommandOrigin const& origin, CommandOutput& output, AmountParams const& args) {
            if (auto* player = getCommandPlayer(origin, output)) {
                auto flag=args.flags ? std::string_view(args.flags->mText) : std::string_view{};
                if(!flag.empty() && flag!="-h" && flag!="-v") {output.error(tr("selection.outset_usage"));return;}
                if (name == "inset") {
                    if(flag.empty()) insetSelection(*player, output, args.amount);
                    else {
                        auto selection=completeSelection(*player,output);
                        if(!selection) return;
                        auto candidate=*selection;
                        bool ok=args.amount>=0;
                        if(flag!="-v") ok=ok && candidate.resize(core::Direction::east,args.amount,args.amount,true) && candidate.resize(core::Direction::south,args.amount,args.amount,true);
                        if(flag!="-h") ok=ok && candidate.resize(core::Direction::up,args.amount,args.amount,true);
                        if(!ok) output.error(tr("selection.contract_empty"));
                        else commitResize(*player,output,*selection,candidate);
                    }
                } else outsetSelection(*player, output, args.amount, flag!="-v", flag!="-h");
            }
        });
        return;
    }
    auto overload = handle.overload();
    if (subcommand) (void)overload.text(name);
    overload.execute([name](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getCommandPlayer(origin, output);
        if (!player) return;
        if (name == "size") {
            if (auto* selection = completeSelection(*player, output)) showSize(*selection, output);
        } else if (name == "desel") {
            clearSelection(*player, output);
        } else if (name == "wand") {
            giveWand(*player, output);
        } else if (name == "togglewand") {
            toggleWand(*player, output);
        } else {
            setLookCorner(*player, name == "hpos1", output);
        }
    });
}

void registerCommands(CommandRegistry& registry) {
    auto& registrar   = ll::command::CommandRegistrar::getServerInstance();
    auto  description = std::string(tr("command.selection"));
    auto& cx = registrar.getOrCreateCommand("cx", std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors);
    for (auto name : commandNames) {
        registerSelectionCommand(cx, name, true);
        if (config.command.shortCommands && !registry.findCommand(std::string(name))) {
            auto& handle =
                registrar.getOrCreateCommand(std::string(name), description, CommandPermissionLevel::GameDirectors);
            registerSelectionCommand(handle, name, false);
        }
    }
}

} // namespace

Player* getCommandPlayer(CommandOrigin const& origin, CommandOutput& output) {
    auto* actor = origin.getEntity();
    if (!actor || !actor->isPlayer()) {
        output.error(tr("command.player_only"));
        return nullptr;
    }
    if (origin.getPermissionsLevel() < CommandPermissionLevel::GameDirectors) {
        output.error(tr("command.no_permission"));
        return nullptr;
    }
    auto* player = static_cast<Player*>(actor);
    noteHudActivity(*player);
    return player;
}

core::CuboidSelection* completeSelection(Player& player, Feedback const& output) {
    auto* session = findSession(player.getUuid());
    if (!session || session->dimension != int(player.getDimensionId())
        || (!session->cuboid.first && !session->cuboid.second)) {
        output.error(tr("selection.none_here"));
        return nullptr;
    }
    auto& selection = session->cuboid;
    if (!selection.first || !selection.second) {
        output.error(tr("selection.missing_corner", selection.first ? 2 : 1));
        return nullptr;
    }
    return &selection;
}

void selectBox(Player& player, core::Box const& box) {
    auto& session     = getSession(player);
    session.dimension = player.getDimensionId();
    session.cuboid    = {box.min, box.max};
    refreshOutline(player, session);
}

bool setCorner(Player& player, bool first, core::Vec3i pos, Feedback const& output) {
    int   dimension = player.getDimensionId();
    auto& session   = getSession(player);
    auto  candidate = session.dimension == dimension ? session.cuboid : core::CuboidSelection{};
    (first ? candidate.first : candidate.second) = pos;
    auto size                                    = candidate.size();
    if (size && !size->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return false;
    }
    session.dimension = dimension;
    session.cuboid    = candidate;
    refreshOutline(player, session);
    output.success(tr("selection.corner", first ? 1 : 2, pos.x, pos.y, pos.z));
    return true;
}

bool setLookCorner(Player& player, bool first, Feedback const& output) {
    auto hit = player.traceRay(config.selection.traceDistance, false, true);
    if (hit.mType != HitResultType::Tile) {
        output.error(tr("selection.no_target", config.selection.traceDistance));
        return false;
    }
    return setCorner(player, first, {hit.mBlock.x, hit.mBlock.y, hit.mBlock.z}, output);
}

bool setSelection(Player& player, core::Vec3i first, core::Vec3i second, Feedback const& output) {
    core::CuboidSelection candidate{first, second};
    if (!candidate.size()->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return false;
    }
    auto& session     = getSession(player);
    session.dimension = player.getDimensionId();
    session.cuboid    = candidate;
    refreshOutline(player, session);
    showSize(candidate, output);
    return true;
}

void showSize(core::CuboidSelection const& selection, Feedback const& output) {
    auto size = *selection.size();
    auto a    = *selection.first;
    auto b    = *selection.second;
    output.success(tr("selection.size", a.x, a.y, a.z, b.x, b.y, b.z, size.x, size.y, size.z, size.volume()));
}

bool resizeSelection(
    Player&         player,
    Feedback const& output,
    bool            contract,
    int             amount,
    int             reverseAmount,
    core::Direction direction
) {
    auto* selection = completeSelection(player, output);
    if (!selection) return false;
    if (amount < 0 || reverseAmount < 0 || (amount == 0 && reverseAmount == 0)) {
        output.error(tr("selection.resize_amount"));
        return false;
    }
    if (direction == core::Direction::me) direction = facingDirection(player);
    auto candidate = *selection;
    if (!candidate.resize(direction, amount, reverseAmount, contract)) {
        output.error(tr(contract ? "selection.contract_empty" : "selection.expand_range"));
        return false;
    }
    return commitResize(player, output, *selection, candidate);
}

bool expandVertically(Player& player, Feedback const& output) {
    auto* selection = completeSelection(player, output);
    if (!selection) return false;
    auto const& range     = *player.getDimension().mHeightRange;
    auto        candidate = *selection;
    candidate.expandVertical(range.mMin, int(range.mMax) - 1);
    return commitResize(player, output, *selection, candidate);
}

bool shiftSelection(Player& player, Feedback const& output, int amount, core::Direction direction) {
    auto* selection = completeSelection(player, output);
    if (!selection) return false;
    if (amount < 0 || direction == core::Direction::me) {
        output.error(tr("selection.shift_invalid"));
        return false;
    }
    core::Vec3i delta{};
    switch (direction) {
    case core::Direction::north: delta.z = -amount; break;
    case core::Direction::south: delta.z = amount; break;
    case core::Direction::east: delta.x = amount; break;
    case core::Direction::west: delta.x = -amount; break;
    case core::Direction::up: delta.y = amount; break;
    case core::Direction::down: delta.y = -amount; break;
    case core::Direction::me: return false;
    }
    auto candidate = *selection;
    for(auto point:{*candidate.first,*candidate.second}) {
        for(auto [coordinate,offset]:{std::pair{point.x,delta.x},std::pair{point.y,delta.y},std::pair{point.z,delta.z}}) {
            auto moved=std::int64_t(coordinate)+offset;
            if(moved<std::numeric_limits<int>::min() || moved>std::numeric_limits<int>::max()) {output.error(tr("selection.expand_range"));return false;}
        }
    }
    candidate.first = *candidate.first + delta;
    candidate.second = *candidate.second + delta;
    if (!candidate.size()->fits(config.limits.maxSelectionVolume)) {
        output.error(tr("selection.too_large", config.limits.maxSelectionVolume));
        return false;
    }
    *selection = candidate;
    refreshOutline(player, getSession(player));
    showSize(candidate, output);
    return true;
}

bool outsetSelection(Player& player, Feedback const& output, int amount, bool horizontal, bool vertical) {
    auto* selection = completeSelection(player, output);
    if (!selection || amount < 0 || (!horizontal && !vertical)) {
        output.error(tr("selection.resize_amount"));
        return false;
    }
    auto candidate = *selection;
    if (horizontal) {
        if (!candidate.resize(core::Direction::east, amount, amount, false)
            || !candidate.resize(core::Direction::south, amount, amount, false)) {
            output.error(tr("selection.expand_range"));
            return false;
        }
    }
    if (vertical && !candidate.resize(core::Direction::up, amount, amount, false)) {
        output.error(tr("selection.expand_range"));
        return false;
    }
    return commitResize(player, output, *selection, candidate);
}

bool insetSelection(Player& player, Feedback const& output, int amount) {
    auto* selection = completeSelection(player, output);
    if (!selection || amount < 0) {
        output.error(tr("selection.resize_amount"));
        return false;
    }
    auto candidate = *selection;
    if (!candidate.resize(core::Direction::east, amount, amount, true)
        || !candidate.resize(core::Direction::south, amount, amount, true)
        || !candidate.resize(core::Direction::up, amount, amount, true)) {
        output.error(tr("selection.contract_empty"));
        return false;
    }
    return commitResize(player, output, *selection, candidate);
}

void clearSelection(Player& player, Feedback const& output) {
    clearSelectionOutline(player);
    if (auto* session = findSession(player.getUuid())) {
        session->cuboid = {};
        session->lastLeftClick.reset();
    }
    output.success(tr("selection.cleared"));
}

bool giveWand(Player& player, Feedback const& output) {
    ItemStack item;
    item.reinit(config.wand.item, 1, 0);
    if (item.isNull()) {
        output.error(tr("wand.invalid_item", config.wand.item));
        return false;
    }
    auto tag = item.mUserData ? std::make_unique<CompoundTag>(*item.mUserData) : std::make_unique<CompoundTag>();
    tag->putBoolean(wandTag, true);
    item.setUserData(std::move(tag));
    item.setCustomName(Bedrock::Safety::RedactableString(std::string(tr("wand.name"))));
    if (!player.add(item)) {
        output.error(tr("wand.inventory_full"));
        return false;
    }
    player.sendInventory(false);
    output.success(tr(config.wand.sneakOpensMenu ? "wand.given_menu" : "wand.given"));
    return true;
}

void toggleWand(Player& player, Feedback const& output) {
    auto& enabled = getSession(player).wandEnabled;
    enabled       = !enabled;
    saveSession(player.getUuid());
    output.success(tr(enabled ? "wand.enabled" : "wand.disabled"));
}

void toggleOutline(Player& player, Feedback const& output) {
    auto& session       = getSession(player);
    session.showOutline = !session.showOutline;
    if (!session.showOutline) clearSelectionOutline(player);
    saveSession(player.getUuid());
    refreshOutline(player, session);
    output.success(tr(session.showOutline ? "selection.outline_on" : "selection.outline_off"));
}

void enableSelectionCommands() {
    auto& bus        = ll::event::EventBus::getInstance();
    registerListener = bus.emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto& event) { registerCommands(event.commandRegistry()); },
        ll::event::EventPriority::Low
    );
    if (auto registry = ll::service::getCommandRegistry(false)) registerCommands(*registry);
    destroyListener = bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>([](auto& event) {
        auto& player = event.self();
        if (!event.isCancelled() && tryWandClick(player, event.pos(), true, player.getSelectedItem())) event.cancel();
    });
    interactListener = bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([](auto& event) {
        if (!event.isCancelled() && tryWandClick(event.self(), event.blockPos(), false, event.item())) event.cancel();
    });
    useItemListener = bus.emplaceListener<ll::event::PlayerUseItemEvent>([](auto& event) {
        auto& player = event.self();
        if (event.isCancelled() || !isWand(player, event.item())) return;
        auto& session = getSession(player);
        if (!session.wandEnabled) return;
        if (tryOpenMenu(player, session)) {
            event.cancel();
        } else if (session.remoteWand) {
            auto hit = player.traceRay(config.selection.traceDistance, false, true);
            if (hit.mType == HitResultType::Tile && tryWandClick(player, hit.mBlock, false, event.item())) event.cancel();
        }
    });
    swingListener = bus.emplaceListener<ll::event::PlayerSwingEvent>([](auto& event) {
        auto& player = event.self();
        auto const& item = player.getSelectedItem();
        if (!isWand(player, item)) return;
        auto& session = getSession(player);
        if (!session.wandEnabled || !session.remoteWand) return;
        auto hit = player.traceRay(config.selection.traceDistance, false, true);
        if (hit.mType == HitResultType::Tile) tryWandClick(player, hit.mBlock, true, item);
    });
    wandStartDestroy.emplace();
    {
        tickListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>([ticks = 0](auto& event) mutable {
            if (++ticks < 30) return;
            ticks = 0;
            if (!hasSessions()) return;
            event.level().forEachPlayer(std::function<bool(Player&)>([](Player& player) {
                auto* session = findSession(player.getUuid());
                if (session && session->dimension == int(player.getDimensionId())) refreshOutline(player, *session);
                return true;
            }));
        });
    }
}

void disableSelectionCommands() {
    auto& bus = ll::event::EventBus::getInstance();
    if (registerListener) bus.removeListener(registerListener);
    if (destroyListener) bus.removeListener(destroyListener);
    if (interactListener) bus.removeListener(interactListener);
    if (useItemListener) bus.removeListener(useItemListener);
    if (swingListener) bus.removeListener(swingListener);
    if (tickListener) bus.removeListener(tickListener);
    wandStartDestroy.reset();
    registerListener.reset();
    destroyListener.reset();
    interactListener.reset();
    useItemListener.reset();
    swingListener.reset();
    tickListener.reset();
}

} // namespace creatorx
