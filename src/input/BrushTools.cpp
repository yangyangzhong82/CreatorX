#include "input/BrushTools.h"

#include "command/EditCommands.h"
#include "command/SelectionCommands.h"
#include "edit/Jobs.h"
#include "mod/Global.h"
#include "net/Enhancements.h"
#include "session/PlayerSession.h"
#include "visual/SelectionOutline.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandRawText.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/HitResult.h"

#include <array>
#include <charconv>

namespace creatorx {

enum class BrushAction { sphere, cyl, smooth, clipboard, gravity };
enum class ToolAction { info, repl };

struct BrushParams {
    BrushAction                           action;
    ll::command::Optional<CommandRawText> arguments;
};
struct ToolParams {
    ToolAction                            action;
    ll::command::Optional<CommandRawText> arguments;
};
struct TextParams {
    ll::command::Optional<CommandRawText> arguments;
};

namespace {

constexpr int  maxRadius     = 64;
constexpr int  maxHeight     = 384;
constexpr int  maxIterations = 10;
constexpr int  maxRange      = 128;
constexpr auto clickInterval = std::chrono::milliseconds(200);
using Clock                  = std::chrono::steady_clock;

ll::event::ListenerPtr commandListener, previewListener;

enum class BindingOption { size, mat, mask, range, none };

bool isTool(BrushBinding const& binding) { return binding.tool == BrushTool::info || binding.tool == BrushTool::repl; }

bool hasRadius(BrushBinding const& binding) { return !isTool(binding) && binding.tool != BrushTool::clipboard; }

bool canUseBindings(Player const& player) {
    return !player.isClientSide() && !player.getLevel().isClientSide()
        && player.getCommandPermissionLevel() >= CommandPermissionLevel::GameDirectors;
}

std::optional<core::Vec3i> traceTarget(Player const& player, int range) {
    auto hit = player.traceRay(float(range), false, true);
    if (hit.mType != HitResultType::Tile) return {};
    return core::Vec3i{hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
}

std::string_view rawText(ll::command::Optional<CommandRawText> const& value) {
    return value ? std::string_view(value->mText) : std::string_view{};
}

bool readNumber(std::string_view token, int& number, int maximum, Feedback const& output) {
    int value{};
    auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || end != token.data() + token.size() || value < 1 || value > maximum) {
        output.error(tr("brush.number", maximum));
        return false;
    }
    number = value;
    return true;
}

bool readPattern(Player& player, Feedback const& output, BrushBinding& binding, std::string_view text) {
    std::string error;
    auto        pattern = edit::Pattern::parse(text, player, error);
    if (!pattern) {
        output.error(error);
        return false;
    }
    binding.pattern = std::move(pattern);
    return true;
}

void bind(Player& player, Feedback const& output, BrushBinding binding, std::string_view name) {
    auto const& item = player.getSelectedItem();
    if (item.isNull()) {
        output.error(tr("brush.no_item"));
        return;
    }
    auto  type        = item.getTypeName();
    auto& session     = getSession(player);
    bool  handlesLeft = isTool(binding);
    session.brushBindings.insert_or_assign(type, std::move(binding));
    if (syncBrushBinding(player, type, handlesLeft)) session.syncedBrushItem = type;
    output.success(tr("brush.bound", name, type));
}

void bindBrush(Player& player, Feedback const& output, BrushAction action, std::string_view raw) {
    auto         tokens = edit::splitTokens(raw);
    BrushBinding binding{};
    size_t       i = 0;
    if (action == BrushAction::clipboard) {
        binding.tool = BrushTool::clipboard;
        for (auto token : tokens) {
            if (token == "-a") binding.skipAir = true;
            else if (token == "-o") binding.original = true;
            else {
                output.error(tr("help.brush"));
                return;
            }
        }
        bind(player, output, std::move(binding), "clipboard");
        return;
    }

    std::string_view name;
    switch (action) {
    case BrushAction::sphere:
        binding.tool = BrushTool::sphere;
        name         = "sphere";
        break;
    case BrushAction::cyl:
        binding.tool = BrushTool::cyl;
        name         = "cyl";
        break;
    case BrushAction::smooth:
        binding.tool = BrushTool::smooth;
        name         = "smooth";
        break;
    case BrushAction::gravity:
        binding.tool = BrushTool::gravity;
        name         = "gravity";
        break;
    case BrushAction::clipboard:
        return;
    }
    bool shape = action == BrushAction::sphere || action == BrushAction::cyl;
    if (shape) {
        if (tokens.empty()) {
            output.error(tr("help.brush"));
            return;
        }
        // 图案可含空格。逗号后的下一项仍属于同一个图案。
        do {
            ++i;
        } while (i < tokens.size() && (tokens[i - 1].ends_with(',') || tokens[i].starts_with(',')));
        auto end = i == tokens.size() ? raw.size() : size_t(tokens[i].data() - raw.data());
        if (!readPattern(player, output, binding, raw.substr(0, end))) return;
    }
    if (i < tokens.size() && !(shape && tokens[i] == "-h")) {
        if (!readNumber(tokens[i++], binding.radius, maxRadius, output)) return;
    }
    if (action == BrushAction::cyl && i < tokens.size() && tokens[i] != "-h") {
        if (!readNumber(tokens[i++], binding.height, maxHeight, output)) return;
    }
    if (action == BrushAction::smooth && i < tokens.size()) {
        if (!readNumber(tokens[i++], binding.iterations, maxIterations, output)) return;
    }
    if (shape && i < tokens.size() && tokens[i] == "-h") {
        binding.hollow = true;
        ++i;
    }
    if (i != tokens.size()) {
        output.error(tr("help.brush"));
        return;
    }
    bind(player, output, std::move(binding), name);
}

void bindTool(Player& player, Feedback const& output, ToolAction action, std::string_view raw) {
    BrushBinding binding{};
    if (action == ToolAction::info) {
        if (!edit::splitTokens(raw).empty()) {
            output.error(tr("help.tool"));
            return;
        }
        binding.tool = BrushTool::info;
        bind(player, output, std::move(binding), "info");
    } else if (readPattern(player, output, binding, raw)) {
        binding.tool = BrushTool::repl;
        bind(player, output, std::move(binding), "repl");
    }
}

void updateBinding(Player& player, Feedback const& output, BindingOption option, std::string_view raw) {
    auto const& item    = player.getSelectedItem();
    auto&       session = getSession(player);
    auto        it      = item.isNull() ? session.brushBindings.end() : session.brushBindings.find(item.getTypeName());
    if (it == session.brushBindings.end()) {
        output.error(tr("brush.no_binding"));
        return;
    }
    auto& binding = it->second;
    auto  tokens  = edit::splitTokens(raw);
    switch (option) {
    case BindingOption::none:
        session.brushBindings.erase(it);
        if (syncBrushBinding(player, {}, false)) session.syncedBrushItem.clear();
        output.success(tr("brush.unbound"));
        return;
    case BindingOption::size:
        if (!hasRadius(binding) || tokens.size() != 1) {
            output.error(tr("help.brush-size"));
            return;
        }
        if (!readNumber(tokens.front(), binding.radius, maxRadius, output)) return;
        break;
    case BindingOption::range:
        if (tokens.size() != 1) {
            output.error(tr("help.range"));
            return;
        }
        if (!readNumber(tokens.front(), binding.range, maxRange, output)) return;
        break;
    case BindingOption::mat:
        if (!binding.pattern) {
            output.error(tr("brush.no_material"));
            return;
        }
        if (!readPattern(player, output, binding, raw)) return;
        break;
    case BindingOption::mask:
        if (isTool(binding)) {
            output.error(tr("brush.mask_only"));
            return;
        }
        if (tokens.empty()) {
            binding.mask.reset();
        } else {
            std::string error;
            auto        mask = edit::Mask::parse(raw, player, error);
            if (!mask) {
                output.error(error);
                return;
            }
            binding.mask = std::move(mask);
        }
        break;
    }
    output.success(tr("brush.updated"));
}

void registerCommands() {
    auto& root = ll::command::CommandRegistrar::getServerInstance()
                     .getOrCreateCommand("cx", std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors);
    root.overload<BrushParams>()
        .text("brush")
        .required("action")
        .optional("arguments")
        .execute([](CommandOrigin const& origin, CommandOutput& output, BrushParams const& params) {
            if (auto* player = getCommandPlayer(origin, output)) {
                bindBrush(*player, output, params.action, rawText(params.arguments));
            }
        });
    root.overload<ToolParams>()
        .text("tool")
        .required("action")
        .optional("arguments")
        .execute([](CommandOrigin const& origin, CommandOutput& output, ToolParams const& params) {
            if (auto* player = getCommandPlayer(origin, output)) {
                bindTool(*player, output, params.action, rawText(params.arguments));
            }
        });
    root.overload().text("none").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (auto* player = getCommandPlayer(origin, output)) updateBinding(*player, output, BindingOption::none, {});
    });
    for (auto [name, option] : std::array{
             std::pair{"size",  BindingOption::size },
             std::pair{"mat",   BindingOption::mat  },
             std::pair{"mask",  BindingOption::mask },
             std::pair{"range", BindingOption::range}
    }) {
        auto overload = root.overload<TextParams>();
        (void)overload.text(name);
        if (option == BindingOption::mask) (void)overload.optional("arguments");
        else (void)overload.required("arguments");
        overload.execute([option](CommandOrigin const& origin, CommandOutput& output, TextParams const& params) {
            if (auto* player = getCommandPlayer(origin, output)) {
                updateBinding(*player, output, option, rawText(params.arguments));
            }
        });
    }
}

bool insideShape(BrushBinding const& binding, core::Vec3i offset) {
    double radius   = binding.radius + 0.5;
    int    distance = offset.x * offset.x + offset.z * offset.z;
    if (binding.tool == BrushTool::sphere) distance += offset.y * offset.y;
    return distance <= radius * radius;
}

void applyBrush(Player& player, Feedback const& output, BrushBinding const& binding, core::Vec3i target) {
    if (binding.tool == BrushTool::clipboard) {
        pasteClipboardAtBrush(player, output, target, binding.skipAir, binding.original, binding.mask);
        return;
    }
    if (binding.pattern && !binding.pattern->isAllowed()) {
        output.error(tr("brush.denied_material"));
        return;
    }
    auto uuid      = player.getUuid();
    int  dimension = player.getDimensionId();
    if (binding.tool == BrushTool::repl) {
        submitEditJob(
            player,
            output,
            edit::makeRegionEdit(uuid, dimension, {target, target}, core::BoxShape::all, *binding.pattern, {}),
            1
        );
        return;
    }

    auto const& range    = *player.getDimension().mHeightRange;
    int         radius   = binding.radius;
    bool        cylinder = binding.tool == BrushTool::cyl;
    core::Box   box{
        {target.x - radius, cylinder ? target.y : std::max(int(range.mMin), target.y - radius),                                   target.z - radius},
        {target.x + radius,
         std::min(int(range.mMax) - 1,                                      target.y + (cylinder ? binding.height - 1 : radius)),
         target.z + radius                                                                                                                         }
    };
    auto estimate = box.volume();
    if (binding.tool == BrushTool::gravity) {
        estimate = std::uint64_t(box.size().x) * box.size().z * (box.max.y - range.mMin + 1);
    }
    if (binding.tool == BrushTool::smooth || binding.tool == BrushTool::gravity) {
        auto job = edit::makeTerrainBrush(
            uuid,
            dimension,
            target,
            box,
            radius,
            binding.tool == BrushTool::smooth ? binding.iterations : 0,
            binding.mask
        );
        submitEditJob(player, output, std::move(job), estimate);
        return;
    }

    auto filter = [binding, target](BlockSource& region, core::Vec3i p) {
        auto offset = p - target;
        if (!insideShape(binding, offset)) return false;
        if (binding.mask && !binding.mask->test(region.getBlock({p.x, p.y, p.z}))) return false;
        if (!binding.hollow) return true;
        constexpr core::Vec3i neighbors[]{
            {1,  0,  0 },
            {-1, 0,  0 },
            {0,  0,  1 },
            {0,  0,  -1},
            {0,  1,  0 },
            {0,  -1, 0 }
        };
        int count = binding.tool == BrushTool::sphere ? 6 : 4;
        return std::any_of(neighbors, neighbors + count, [&](core::Vec3i step) {
            return !insideShape(binding, offset + step);
        });
    };
    auto job = edit::makeComputedEdit(
        uuid,
        dimension,
        box,
        *binding.pattern,
        std::move(filter),
        cylinder ? "brush cyl" : "brush sphere"
    );
    submitEditJob(player, output, std::move(job), estimate);
}

void inspectBlock(Feedback const& output, BlockSource const& region, BlockPos const& pos, Block const& block) {
    auto const& definition = *block.mSerializationId;
    std::string states;
    if (definition.contains("states")) {
        for (auto const& [key, value] : definition["states"].get<CompoundTag>().mTags) {
            if (!states.empty()) states += ", ";
            states += key;
            states += '=';
            if (value.getId() == Tag::Type::Byte) states += bool(value) ? "true" : "false";
            else states += value.toSnbt(SnbtFormat::Minimize);
        }
    }
    output.success(tr("brush.block_info", pos.x, pos.y, pos.z, block.getTypeName(), states));
    if (auto* entity = region.getBlockEntity(pos)) {
        CompoundTag tag;
        if (entity->save(tag, SaveContext{})) output.success(tr("brush.block_nbt", tag.toSnbt(SnbtFormat::Minimize)));
    }
}

bool updatePreview(Player& player) {
    auto* session = findSession(player.getUuid());
    if (!session || (session->brushBindings.empty() && session->syncedBrushItem.empty())) return true;
    auto const& item = player.getSelectedItem();
    auto        type = item.isNull() ? std::string{} : item.getTypeName();
    auto        it   = session->brushBindings.find(type);
    if (!canUseBindings(player) || it == session->brushBindings.end()) {
        if (!session->syncedBrushItem.empty() && syncBrushBinding(player, {}, false)) session->syncedBrushItem.clear();
        return true;
    }
    auto const& binding = it->second;
    if (session->syncedBrushItem != type && syncBrushBinding(player, type, isTool(binding))) {
        session->syncedBrushItem = std::move(type);
    }
    if (isTool(binding) || config.visual.mode == "none" || config.visual.mode == "particles"
        || config.visual.maxDistance <= 0 || !hasEnhancedClient(player))
        return true;
    if (auto target = traceTarget(player, binding.range)) showBrushTarget(player, *target);
    return true;
}

} // namespace

bool handleBrushClick(Player& player, ItemStack const& item, bool left) {
    if (!canUseBindings(player) || item.isNull()) return false;
    auto* session = findSession(player.getUuid());
    if (!session || session->brushBindings.empty()) return false;
    auto type = item.getTypeName();
    auto it   = session->brushBindings.find(type);
    if (it == session->brushBindings.end() || (left && !isTool(it->second))) return false;
    auto const& selected = player.getSelectedItem();
    if (&selected != &item && (selected.isNull() || selected.getTypeName() != type)) return false;

    auto  now  = Clock::now();
    auto& last = left ? session->lastBrushLeft : session->lastBrushRight;
    if (now - last < clickInterval) return true;
    last = now;

    auto&    binding = it->second;
    Feedback output(player);
    auto     target = traceTarget(player, binding.range);
    if (!target) {
        output.error(tr("selection.no_target", binding.range));
        return true;
    }
    if (binding.tool == BrushTool::info || (binding.tool == BrushTool::repl && left)) {
        auto const& region = player.getDimensionBlockSource();
        BlockPos    pos{target->x, target->y, target->z};
        auto const& block = region.getBlock(pos);
        if (binding.tool == BrushTool::info) inspectBlock(output, region, pos, block);
        else if (edit::isDisallowed(block)) output.error(tr("pattern.disallowed", block.getTypeName()));
        else {
            binding.pattern = edit::Pattern::single(block);
            output.success(tr("brush.sampled", block.getTypeName()));
        }
    } else {
        applyBrush(player, output, binding, *target);
    }
    return true;
}

void enableBrushTools() {
    auto& bus       = ll::event::EventBus::getInstance();
    commandListener = bus.emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto&) { registerCommands(); },
        ll::event::EventPriority::Low
    );
    if (ll::service::getCommandRegistry(false)) registerCommands();
    previewListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>([ticks = 0](auto& event) mutable {
        if (++ticks < 3) return;
        ticks = 0;
        if (hasSessions()) event.level().forEachPlayer(std::function<bool(Player&)>(updatePreview));
    });
}

void disableBrushTools() {
    auto& bus = ll::event::EventBus::getInstance();
    if (commandListener) bus.removeListener(commandListener);
    if (previewListener) bus.removeListener(previewListener);
    commandListener.reset();
    previewListener.reset();
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer(std::function<bool(Player&)>([](Player& player) {
            auto* session = findSession(player.getUuid());
            if (session && !session->syncedBrushItem.empty()) syncBrushBinding(player, {}, false);
            return true;
        }));
    }
}

} // namespace creatorx
