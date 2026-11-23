#include "command/EditCommands.h"

#include "command/SelectionCommands.h"
#include "edit/Jobs.h"
#include "edit/Pattern.h"
#include "Config/ConfigManager.h"
#include "visual/PastePreview.h"
#include "visual/SelectionOutline.h"
#include "net/Enhancements.h"
#include "exec/JobScheduler.h"
#include "mod/Global.h"
#include "session/PlayerSession.h"
#include "ui/Menu.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandRawText.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/Level.h"

#include <array>
#include <functional>
#include <limits>

namespace creatorx {

enum class CreatorXBlockId {};
using BlockId = ll::command::SoftEnum<CreatorXBlockId>;

struct BlockParams {
    BlockId block;
};

struct PatternParams {
    CommandRawText pattern;
};

struct OptionalPatternParams {
    ll::command::Optional<CommandRawText> pattern;
};

struct FlagsParams {
    ll::command::Optional<CommandRawText> flags;
};

struct TimesParams {
    ll::command::Optional<int> times;
    ll::command::Optional<CommandRawText> player;
};

struct RotateParams {
    int quarterTurns;
    ll::command::Optional<int> x;
    ll::command::Optional<int> z;
};

struct FlipParams {
    ll::command::Optional<core::Direction> direction;
};
struct HelpParams { ll::command::Optional<CommandRawText> topic; };

enum class SimpleEditAction { cancel, clearclipboard, clearhistory, menu, reload, history, confirm };

struct PreviewAdjustParams {
    int x;
    int y;
    int z;
    ll::command::Optional<int> rotation;
};

struct SimpleEditParams {
    SimpleEditAction action;
};

namespace {

constexpr std::array<std::string_view, 14> commandNames{
    "set",
    "replace",
    "walls",
    "faces",
    "outline",
    "copy",
    "cut",
    "paste",
    "undo",
    "redo",
    "help",
    "rotate",
    "flip",
    "pastepreview"
};
ll::event::ListenerPtr registerListener;
std::uint64_t generation=0;

// 检查改动上限，必要时弹窗确认，然后提交任务。返回任务是否已提交或已发出确认窗口。
// onSubmitted 在任务真正提交时调用（确认窗口里点了执行之后）。
bool startJob(
    Player&                       player,
    Feedback const&               output,
    std::unique_ptr<exec::Job>    job,
    std::uint64_t                 changes,
    std::function<void(Player&)> onSubmitted = {},
    bool confirmed = false
) {
    std::string error;
    if (!exec::canSubmit(player.getUuid(), error)) {
        output.error(error);
        return false;
    }
    if (changes > config.limits.maxChangesPerOperation) {
        output.error(tr("job.too_many_changes", changes, config.limits.maxChangesPerOperation));
        return false;
    }
    if (changes <= config.limits.confirmThreshold || confirmed) {
        if (!exec::submitJob(player, std::move(job), error)) {
            output.error(error);
            return false;
        }
        if (onSubmitted) onSubmitted(player);
        output.success(tr("job.started", changes));
        return true;
    }

    auto pending = std::make_shared<std::unique_ptr<exec::Job>>(std::move(job));
    ll::form::ModalForm form(
        std::string(tr("job.confirm_title")),
        tr("job.confirm_content", changes),
        std::string(tr("job.confirm_yes")),
        std::string(tr("job.confirm_no"))
    );
    form.sendTo(
        player,
        [pending, onSubmitted, dimension=int(player.getDimensionId()), expectedGeneration=generation](Player& target, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
            bool valid=dimension==int(target.getDimensionId()) && generation==expectedGeneration
                && target.getCommandPermissionLevel()>=CommandPermissionLevel::GameDirectors;
            if(!valid) { target.sendMessage(tr("preview.expired")); return; }
            if (result != ll::form::ModalFormSelectedButton::Upper || !*pending) {
                target.sendMessage(tr("job.confirm_declined"));
                return;
            }
            std::string error;
            if (!exec::submitJob(target, std::move(*pending), error)) {
                target.sendMessage(error);
                return;
            }
            if (onSubmitted) onSubmitted(target);
            target.sendMessage(tr("job.started_confirmed"));
        }
    );
    output.success(tr("job.confirm_sent", changes));
    return true;
}

bool isHandPattern(std::string_view text) { return text == "hand" || text == "offhand"; }

std::optional<PasteOptions> parsePasteFlags(std::string_view flags, Feedback const& output) {
    PasteOptions options;
    bool nextMask=false;
    for (auto token : edit::splitTokens(flags)) {
        if(nextMask) {options.mask=std::string(flags.substr(token.data()-flags.data()));break;}
        if(token=="-m") {nextMask=true;continue;}
        if (token.size() < 2 || token.front() != '-') {
            output.error(tr("clipboard.bad_flag", token));
            return std::nullopt;
        }
        for (char flag : token.substr(1)) {
            if (flag == 'a') options.skipAir = true;
            else if (flag == 'o') options.original = true;
            else if (flag == 's') options.select = true;
            else if (flag == 'p') options.preview = true;
            else if (flag == 'e') options.entities = true;
            else {
                output.error(tr("clipboard.bad_flag", token));
                return std::nullopt;
            }
        }
    }
    if(nextMask && !options.mask) {output.error(tr("mask.empty"));return std::nullopt;}
    return options;
}

template <class Params = ll::command::EmptyParam>
auto subOverload(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    auto overload = handle.overload<Params>();
    if (subcommand) (void)overload.text(name);
    return overload;
}

void executeSimpleEdit(SimpleEditAction action, CommandOrigin const& origin, CommandOutput& output) {
    auto* player = getCommandPlayer(origin, output);
    if (!player) return;
    switch (action) {
    case SimpleEditAction::cancel:
        cancelPlayerJob(*player, output);
        break;
    case SimpleEditAction::clearclipboard:
        clearClipboard(*player, output);
        break;
    case SimpleEditAction::clearhistory:
        clearHistory(*player, output);
        break;
    case SimpleEditAction::menu:
        ui::openMainMenu(*player);
        output.success(tr("menu.opened"));
        break;
    case SimpleEditAction::reload:
        reloadConfiguration(output);
        break;
    case SimpleEditAction::history:
        output.success(getSession(*player).history.describe());
        break;
    case SimpleEditAction::confirm:
        confirmPastePreview(*player, output);
        break;
    }
}

void registerEditCommand(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    using Origin = CommandOrigin const&;
    using Output = CommandOutput&;

    if (name == "set") {
        subOverload<BlockParams>(handle, name, subcommand)
            .required("block")
            .execute([](Origin origin, Output output, BlockParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    regionEdit(*player, output, core::BoxShape::all, args.block, std::nullopt);
                }
            });
    }
    if (name == "set" || name == "walls" || name == "faces" || name == "outline") {
        auto shape = name == "set" ? core::BoxShape::all : name == "walls" ? core::BoxShape::walls : core::BoxShape::faces;
        subOverload<PatternParams>(handle, name, subcommand)
            .required("pattern")
            .execute([shape](Origin origin, Output output, PatternParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    regionEdit(*player, output, shape, args.pattern.mText, std::nullopt);
                }
            });
    } else if (name == "replace") {
        subOverload<PatternParams>(handle, name, subcommand)
            .required("pattern")
            .execute([](Origin origin, Output output, PatternParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) replaceBlocks(*player, output, args.pattern.mText);
            });
    } else if (name == "copy") {
        subOverload<FlagsParams>(handle, name, subcommand).optional("flags").execute([](Origin origin, Output output, FlagsParams const& args) {
            if (auto* player = getCommandPlayer(origin, output)) {
                auto options=parsePasteFlags(args.flags ? std::string_view(args.flags->mText) : std::string_view{},output);
                if(options && (options->skipAir || options->original || options->select || options->preview)) {output.error(tr("clipboard.copy_usage"));return;}
                if(options) copySelection(*player,output,false,{},options->mask ? std::optional<std::string_view>(*options->mask) : std::nullopt,options->entities);
            }
        });
    } else if (name == "cut") {
        subOverload<OptionalPatternParams>(handle, name, subcommand)
            .optional("pattern")
            .execute([](Origin origin, Output output, OptionalPatternParams const& args) {
                auto* player = getCommandPlayer(origin, output);
                if (!player) return;
                std::string pattern;
                std::optional<std::string_view> mask;
                bool entities=false;
                if(args.pattern) {
                    auto text=std::string_view(args.pattern->mText);
                    auto tokens=edit::splitTokens(text);
                    for(size_t i=0;i<tokens.size();++i) {
                        if(tokens[i]=="-e") entities=true;
                        else if(tokens[i]=="-m") {
                            if(++i>=tokens.size()) {output.error(tr("mask.empty"));return;}
                            mask=text.substr(tokens[i].data()-text.data()); break;
                        } else {
                            if (!pattern.empty()) pattern += ' ';
                            pattern += tokens[i];
                        }
                    }
                }
                auto patternText = pattern.empty() ? std::nullopt : std::optional<std::string_view>{pattern};
                copySelection(*player, output, true, patternText, mask, entities);
            });
    } else if (name == "paste") {
        subOverload<FlagsParams>(handle, name, subcommand)
            .optional("flags")
            .execute([](Origin origin, Output output, FlagsParams const& args) {
                auto* player = getCommandPlayer(origin, output);
                if (!player) return;
                auto options =
                    parsePasteFlags(args.flags ? std::string_view(args.flags->mText) : std::string_view{}, output);
                if (options) pasteClipboard(*player, output, *options);
            });
    } else if (name == "undo" || name == "redo") {
        subOverload<TimesParams>(handle, name, subcommand)
            .optional("times").optional("player")
            .execute([undo = name == "undo"](Origin origin, Output output, TimesParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    runHistory(*player, output, undo, args.times.value_or(1),args.player ? std::string_view(args.player->mText) : std::string_view{});
                }
            });
    } else if (name == "rotate") {
        subOverload<RotateParams>(handle, name, subcommand)
            .required("quarterTurns").optional("x").optional("z")
            .execute([](Origin origin, Output output, RotateParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) rotateClipboard(*player, output, args.quarterTurns,args.x.value_or(0),args.z.value_or(0));
            });
    } else if (name == "flip") {
        subOverload<FlipParams>(handle, name, subcommand)
            .optional("direction")
            .execute([](Origin origin, Output output, FlipParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    auto direction = args.direction.value_or(core::Direction::me);
                    flipClipboard(*player, output, direction);
                }
            });
    } else if (name == "pastepreview") {
        subOverload<PreviewAdjustParams>(handle, name, subcommand)
            .required("x").required("y").required("z").optional("rotation")
            .execute([](Origin origin, Output output, PreviewAdjustParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    adjustPastePreview(*player, output, {args.x, args.y, args.z}, args.rotation.value_or(0));
                }
            });
    } else if(name=="help") {
        subOverload<HelpParams>(handle,name,subcommand).optional("topic").execute([](Origin origin,Output output,HelpParams const& args) {
            if(!getCommandPlayer(origin,output)) return;
            auto key=args.topic ? "help."+args.topic->mText : "help.index";
            auto message=tr(key);
            if(message==key) output.error(tr("help.unknown"));
            else output.success(message);
        });
    }
}

void registerCommands(CommandRegistry& registry) {
    auto& registrar = ll::command::CommandRegistrar::getServerInstance();
    auto blockIds = edit::registeredBlockIds();
    auto fullIds = blockIds.size();
    for (size_t i = 0; i < fullIds; ++i) {
        if (blockIds[i].starts_with("minecraft:")) blockIds.push_back(blockIds[i].substr(10));
    }
    constexpr auto enumName = ll::command::enum_name_v<BlockId>;
    if (registrar.hasSoftEnum(enumName)) registrar.setSoftEnumValues(enumName, std::move(blockIds));
    else registrar.tryRegisterSoftEnum(enumName, std::move(blockIds));

    auto& cx = registrar.getOrCreateCommand("cx", std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors);
    // 共用一个重载，减少进入存档时原版解析器注册的规则。
    cx.overload<SimpleEditParams>().required("action").execute(
        [](CommandOrigin const& origin, CommandOutput& output, SimpleEditParams const& args) {
            executeSimpleEdit(args.action, origin, output);
        }
    );

    auto registerShortCommand = [&](std::string_view name, auto&& addOverloads) {
        if (!config.command.shortCommands || registry.findCommand(std::string(name))) return;
        auto description = std::string(tr(fmt::format("command.{}", name)));
        auto& handle =
            registrar.getOrCreateCommand(std::string(name), description, CommandPermissionLevel::GameDirectors);
        addOverloads(handle);
    };
    for (auto name : commandNames) {
        registerEditCommand(cx, name, true);
        registerShortCommand(name, [&](ll::command::CommandHandle& handle) {
            registerEditCommand(handle, name, false);
        });
    }
    for (auto action : magic_enum::enum_values<SimpleEditAction>()) {
        registerShortCommand(magic_enum::enum_name(action), [action](ll::command::CommandHandle& handle) {
            handle.overload().execute([action](CommandOrigin const& origin, CommandOutput& output) {
                executeSimpleEdit(action, origin, output);
            });
        });
    }
}

} // namespace

bool submitEditJob(Player& player, Feedback const& output, std::unique_ptr<exec::Job> job, std::uint64_t changes,std::function<void(Player&)> onSubmitted) {
    return startJob(player, output, std::move(job), changes,std::move(onSubmitted));
}

bool reloadConfiguration(Feedback const& output) {
    if(!ConfigManager::getInstance().reload()) {output.error(tr("config.load_error"));return false;}
    I18n::getInstance().setLanguage(config.language);
    edit::refreshBlockBlacklist();
    refreshEnhancementOptions();
    if(auto level=ll::service::getLevel()) level->forEachPlayer(std::function<bool(Player&)>([](Player& player) {
        clearSelectionOutline(player);
        cancelPastePreview(player);
        if(auto session=findSession(player.getUuid())) if(session->showOutline && session->dimension==int(player.getDimensionId())) showSelectionOutline(player,session->cuboid);
        return true;
    }));
    output.success(tr("config.reloaded"));
    return true;
}

bool regionEdit(
    Player&                         player,
    Feedback const&                 output,
    core::BoxShape                  shape,
    std::string_view                patternText,
    std::optional<std::string_view> maskText
) {
    auto* selection = completeSelection(player, output);
    if (!selection) return false;
    std::string error;
    auto        pattern = edit::Pattern::parse(patternText, player, error);
    if (!pattern) {
        output.error(error);
        return false;
    }
    std::optional<edit::Mask> mask;
    if (maskText) {
        mask = edit::Mask::parse(*maskText, player, error);
        if (!mask) {
            output.error(error);
            return false;
        }
    }
    auto box = *selection->box();
    auto job = edit::makeRegionEdit(
        player.getUuid(),
        player.getDimensionId(),
        box,
        shape,
        std::move(*pattern),
        std::move(mask)
    );
    return startJob(player, output, std::move(job), core::shapeVolume(box, shape),
        [pattern=std::string(patternText),mask=maskText ? std::optional<std::string>(*maskText) : std::nullopt](Player& target) {
            auto& session=getSession(target);
            if(!isHandPattern(pattern)) rememberRecent(session.recentPatterns,pattern);
            if(mask && *mask!="#existing" && !isHandPattern(*mask)) rememberRecent(session.recentMasks,*mask);
            saveSession(target.getUuid());
        });
}

bool replaceBlocks(Player& player, Feedback const& output, std::string_view text) {
    auto tokens = edit::splitTokens(text);
    if (tokens.empty()) {
        output.error(tr("edit.replace_usage"));
        return false;
    }
    // 最后一个图案可能写成 `60%stone, dirt`，逗号后的空格不算分隔。
    auto first = tokens.size() - 1;
    while (first > 0 && tokens[first - 1].ends_with(',')) --first;
    auto start  = size_t(tokens[first].data() - text.data());
    auto target = text.substr(start);
    auto mask   = first > 0 ? text.substr(0, start) : std::string_view("#existing");
    return regionEdit(player, output, core::BoxShape::all, target, mask);
}

bool copySelection(Player& player, Feedback const& output, bool cut, std::optional<std::string_view> patternText, std::optional<std::string_view> maskText, bool includeEntities) {
    auto* selection = completeSelection(player, output);
    if (!selection) return false;
    std::optional<edit::Pattern> pattern;
    if (cut) {
        std::string error;
        pattern = patternText ? edit::Pattern::parse(*patternText, player, error) : edit::Pattern::single(edit::airBlock());
        if (!pattern) {
            output.error(error);
            return false;
        }
    }
    std::optional<edit::Mask> mask;
    if(maskText) {
        std::string error;
        mask=edit::Mask::parse(*maskText,player,error);
        if(!mask) {output.error(error);return false;}
    }
    auto box    = *selection->box();
    auto feet   = player.getFeetBlockPos();
    auto offset = box.min - core::Vec3i{feet.x, feet.y, feet.z};
    auto job    = edit::makeCopy(player.getUuid(), player.getDimensionId(), box, offset, std::move(pattern),std::move(mask),includeEntities);
    // 复制不改动世界，只有剪切需要计入改动上限。
    return startJob(player, output, std::move(job), cut ? box.volume() : 0);
}

namespace {
std::optional<core::Box> placementBox(
    Player& player, Feedback const& output, core::Vec3i base, core::Vec3i offset, core::Vec3i size
) {
    auto x = std::int64_t(base.x) + offset.x;
    auto y = std::int64_t(base.y) + offset.y;
    auto z = std::int64_t(base.z) + offset.z;
    auto const& height = *player.getDimension().mHeightRange;
    if (y < height.mMin || y + size.y > height.mMax) {
        output.error(tr("clipboard.out_of_height"));
        return {};
    }
    if (x < std::numeric_limits<int>::min() || z < std::numeric_limits<int>::min()
        || x + size.x > std::numeric_limits<int>::max() || z + size.z > std::numeric_limits<int>::max()) {
        output.error(tr("selection.expand_range"));
        return {};
    }
    return core::Box{{int(x), int(y), int(z)}, {int(x + size.x - 1), int(y + size.y - 1), int(z + size.z - 1)}};
}

bool submitPaste(Player& player, Feedback const& output, PastePlacement placement, core::Box box, bool confirmed) {
    auto changes = placement.clipboard->volume();
    auto job = edit::makePaste(
        player.getUuid(), player.getDimensionId(), std::move(placement.clipboard), box.min,
        placement.skipAir, std::move(placement.mask), placement.entities
    );
    auto onSubmitted = [box, select = placement.select](Player& target) {
        cancelPastePreview(target);
        if (select) selectBox(target, box);
    };
    return startJob(player, output, std::move(job), changes, onSubmitted, confirmed);
}
} // namespace

bool pasteClipboard(Player& player, Feedback const& output, PasteOptions options) {
    auto clipboard = getSession(player).clipboard;
    if (!clipboard) {
        output.error(tr("clipboard.empty"));
        return false;
    }
    auto feet = player.getFeetBlockPos();
    auto box = placementBox(player, output, options.original ? clipboard->origin : core::Vec3i{feet.x, feet.y, feet.z},
                            options.original ? core::Vec3i{} : clipboard->offset, clipboard->size);
    if (!box) return false;
    std::optional<edit::Mask> mask;
    if(options.mask) {
        std::string error;
        mask=edit::Mask::parse(*options.mask,player,error);
        if(!mask) {output.error(error);return false;}
    }
    PastePlacement placement{clipboard, box->min, options.skipAir, options.select, options.entities, std::move(mask)};
    if (!options.preview) return submitPaste(player, output, std::move(placement), *box, false);
    if (clipboard->volume() > config.limits.maxChangesPerOperation) {
        output.error(tr("job.too_many_changes", clipboard->volume(), config.limits.maxChangesPerOperation));
        return false;
    }
    showPastePreview(player, std::move(placement));
    output.success(tr("preview.ready", config.visual.previewSeconds));
    return true;
}

bool confirmPastePreview(Player& player, Feedback const& output) {
    auto placement = getPastePreview(player);
    if (!placement) {
        output.error(tr("preview.none"));
        return false;
    }
    auto box = placementBox(player, output, placement->min, {}, placement->clipboard->size);
    return box && submitPaste(player, output, *placement, *box, true);
}

bool adjustPastePreview(Player& player, Feedback const& output, core::Vec3i offset, int rotation) {
    auto current = getPastePreview(player);
    if (!current) {
        output.error(tr("preview.none"));
        return false;
    }
    if (rotation % 90 != 0) {
        output.error(tr("clipboard.rotation_invalid"));
        return false;
    }
    auto placement = *current;
    try {
        placement.clipboard = edit::rotateClipboard(placement.clipboard, rotation / 90);
    } catch (std::exception const& error) {
        output.error(tr("advanced.failed", error.what()));
        return false;
    }
    auto box = placementBox(player, output, placement.min, offset, placement.clipboard->size);
    if (!box) return false;
    placement.min = box->min;
    showPastePreview(player, std::move(placement));
    output.success(tr("preview.moved", box->min.x, box->min.y, box->min.z, config.visual.previewSeconds));
    return true;
}

bool runHistory(Player& player, Feedback const& output, bool undo, int times, std::string_view target) {
    if (times < 1) {
        output.error(tr("history.bad_times"));
        return false;
    }
    std::string error;
    if (!exec::canSubmit(player.getUuid(), error)) {
        output.error(error);
        return false;
    }
    auto uuid=player.getUuid();
    if(!target.empty()) {
        if(auto other=player.getLevel().getPlayer(std::string(target))) uuid=other->getUuid();
        else {
            auto parsed=mce::UUID::tryFromString(target);
            if(!parsed) {output.error(tr("history.player_missing"));return false;}
            uuid=*parsed;
        }
    }
    if(uuid!=player.getUuid() && exec::hasJob(uuid)) {output.error(tr("job.busy"));return false;}
    auto& stack = getSession(uuid).history;
    auto  sets  = undo ? stack.popUndo(size_t(times)) : stack.popRedo(size_t(times));
    if (sets.empty()) {
        output.error(tr(undo ? "history.nothing_to_undo" : "history.nothing_to_redo"));
        return false;
    }
    auto job = edit::makeHistory(uuid, undo, std::move(sets),player.getUuid());
    if (!exec::submitJob(player, std::move(job), error,uuid)) {
        // 提交失败时任务被销毁，finish 不会把记录放回去；canSubmit 已检查过，正常走不到这里。
        output.error(error);
        return false;
    }
    output.success(tr(undo ? "history.undo_started" : "history.redo_started"));
    return true;
}

void cancelPlayerJob(Player& player, Feedback const& output) {
    if (cancelPastePreview(player)) output.success(tr("preview.cancelled"));
    else if (exec::cancelJob(player.getUuid())) output.success(tr("job.cancel_requested"));
    else output.error(tr("job.none"));
}

void clearClipboard(Player& player, Feedback const& output) {
    getSession(player).clipboard.reset();
    saveClipboard(player.getUuid());
    output.success(tr("clipboard.cleared"));
}

void clearHistory(Player& player, Feedback const& output) {
    getSession(player).history.clear();
    saveSession(player.getUuid());
    output.success(tr("history.cleared"));
}

bool rotateClipboard(Player& player, Feedback const& output, int quarterTurns,int x,int z) {
    auto& clipboard = getSession(player).clipboard;
    if (!clipboard) {
        output.error(tr("clipboard.empty"));
        return false;
    }
    if (quarterTurns % 90 != 0 || x%90!=0 || z%90!=0) { output.error(tr("clipboard.rotation_invalid")); return false; }
    try {
        auto result=edit::rotateClipboard(clipboard,quarterTurns/90,1);
        result=edit::rotateClipboard(result,x/90,0);
        result=edit::rotateClipboard(result,z/90,2);
        clipboard=std::move(result);
    }
    catch (std::exception const& error) { output.error(tr("advanced.failed", error.what())); return false; }
    output.success(tr("clipboard.rotated",quarterTurns,x,z));
    saveClipboard(player.getUuid());
    return true;
}

bool flipClipboard(Player& player, Feedback const& output, core::Direction direction) {
    auto& clipboard = getSession(player).clipboard;
    if (!clipboard) {
        output.error(tr("clipboard.empty"));
        return false;
    }
    if (direction == core::Direction::me) direction = facingDirection(player);
    try { clipboard = edit::flipClipboard(clipboard, direction); }
    catch (std::exception const& error) { output.error(tr("advanced.failed", error.what())); return false; }
    output.success(tr("clipboard.flipped"));
    saveClipboard(player.getUuid());
    return true;
}

void enableEditCommands() {
    ++generation;
    registerListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto& event) { registerCommands(event.commandRegistry()); },
        ll::event::EventPriority::Low
    );
    if (auto registry = ll::service::getCommandRegistry(false)) registerCommands(*registry);
}

void disableEditCommands() {
    ++generation;
    if (registerListener) ll::event::EventBus::getInstance().removeListener(registerListener);
    registerListener.reset();
}

} // namespace creatorx
