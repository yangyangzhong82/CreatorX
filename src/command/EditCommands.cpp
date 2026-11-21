#include "command/EditCommands.h"

#include "command/SelectionCommands.h"
#include "edit/Jobs.h"
#include "edit/Pattern.h"
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

#include <array>
#include <functional>

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
    "cancel",
    "clearclipboard",
    "clearhistory",
    "menu"
};
ll::event::ListenerPtr registerListener;

// 检查改动上限，必要时弹窗确认，然后提交任务。返回任务是否已提交或已发出确认窗口。
// onSubmitted 在任务真正提交时调用（确认窗口里点了执行之后）。
bool startJob(
    Player&                       player,
    Feedback const&               output,
    std::unique_ptr<exec::Job>    job,
    std::uint64_t                 changes,
    std::function<void(Player&)> onSubmitted = {}
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
    if (changes <= config.limits.confirmThreshold) {
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
        [pending, onSubmitted](Player& target, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
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
    for (auto token : edit::splitTokens(flags)) {
        if (token.size() < 2 || token.front() != '-') {
            output.error(tr("clipboard.bad_flag", token));
            return std::nullopt;
        }
        for (char flag : token.substr(1)) {
            if (flag == 'a') options.skipAir = true;
            else if (flag == 'o') options.original = true;
            else if (flag == 's') options.select = true;
            else {
                output.error(tr("clipboard.bad_flag", token));
                return std::nullopt;
            }
        }
    }
    return options;
}

template <class Params = ll::command::EmptyParam>
auto subOverload(ll::command::CommandHandle& handle, std::string_view name, bool subcommand) {
    auto overload = handle.overload<Params>();
    if (subcommand) (void)overload.text(name);
    return overload;
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
        subOverload(handle, name, subcommand).execute([](Origin origin, Output output) {
            if (auto* player = getCommandPlayer(origin, output)) copySelection(*player, output, false, std::nullopt);
        });
    } else if (name == "cut") {
        subOverload<OptionalPatternParams>(handle, name, subcommand)
            .optional("pattern")
            .execute([](Origin origin, Output output, OptionalPatternParams const& args) {
                auto* player = getCommandPlayer(origin, output);
                if (!player) return;
                std::optional<std::string_view> pattern;
                if (args.pattern) pattern = args.pattern->mText;
                copySelection(*player, output, true, pattern);
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
            .optional("times")
            .execute([undo = name == "undo"](Origin origin, Output output, TimesParams const& args) {
                if (auto* player = getCommandPlayer(origin, output)) {
                    runHistory(*player, output, undo, args.times.value_or(1));
                }
            });
    } else {
        subOverload(handle, name, subcommand).execute([name](Origin origin, Output output) {
            auto* player = getCommandPlayer(origin, output);
            if (!player) return;
            if (name == "cancel") {
                cancelPlayerJob(*player, output);
            } else if (name == "clearclipboard") {
                clearClipboard(*player, output);
            } else if (name == "clearhistory") {
                clearHistory(*player, output);
            } else {
                ui::openMainMenu(*player);
                output.success(tr("menu.opened"));
            }
        });
    }
}

void registerCommands() {
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
    for (auto name : commandNames) {
        registerEditCommand(cx, name, true);
        if (config.command.shortCommands) {
            auto description = std::string(tr(fmt::format("command.{}", name)));
            auto& handle =
                registrar.getOrCreateCommand(std::string(name), description, CommandPermissionLevel::GameDirectors);
            registerEditCommand(handle, name, false);
        }
    }
}

} // namespace

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
    if (!startJob(player, output, std::move(job), core::shapeVolume(box, shape))) return false;

    // 只记录成功提交的图案，表单下拉框里“最近使用”才可靠。
    auto& session = getSession(player);
    if (!isHandPattern(patternText)) rememberRecent(session.recentPatterns, patternText);
    if (maskText && *maskText != "#existing" && !isHandPattern(*maskText)) {
        rememberRecent(session.recentMasks, *maskText);
    }
    return true;
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

bool copySelection(Player& player, Feedback const& output, bool cut, std::optional<std::string_view> patternText) {
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
    auto box    = *selection->box();
    auto feet   = player.getFeetBlockPos();
    auto offset = box.min - core::Vec3i{feet.x, feet.y, feet.z};
    auto job    = edit::makeCopy(player.getUuid(), player.getDimensionId(), box, offset, std::move(pattern));
    // 复制不改动世界，只有剪切需要计入改动上限。
    return startJob(player, output, std::move(job), cut ? box.volume() : 0);
}

bool pasteClipboard(Player& player, Feedback const& output, PasteOptions options) {
    auto clipboard = getSession(player).clipboard;
    if (!clipboard) {
        output.error(tr("clipboard.empty"));
        return false;
    }
    auto feet = player.getFeetBlockPos();
    auto min  = options.original ? clipboard->origin : core::Vec3i{feet.x, feet.y, feet.z} + clipboard->offset;
    auto box  = core::Box{min, min + clipboard->size - core::Vec3i{1, 1, 1}};

    auto const& range = *player.getDimension().mHeightRange;
    if (box.min.y < range.mMin || box.max.y >= range.mMax) {
        output.error(tr("clipboard.out_of_height"));
        return false;
    }
    auto job = edit::makePaste(player.getUuid(), player.getDimensionId(), clipboard, min, options.skipAir);
    std::function<void(Player&)> select;
    if (options.select) select = [box](Player& target) { selectBox(target, box); };
    return startJob(player, output, std::move(job), clipboard->volume(), std::move(select));
}

bool runHistory(Player& player, Feedback const& output, bool undo, int times) {
    if (times < 1) {
        output.error(tr("history.bad_times"));
        return false;
    }
    std::string error;
    if (!exec::canSubmit(player.getUuid(), error)) {
        output.error(error);
        return false;
    }
    auto& stack = getSession(player).history;
    auto  sets  = undo ? stack.popUndo(size_t(times)) : stack.popRedo(size_t(times));
    if (sets.empty()) {
        output.error(tr(undo ? "history.nothing_to_undo" : "history.nothing_to_redo"));
        return false;
    }
    auto job = edit::makeHistory(player.getUuid(), undo, std::move(sets));
    if (!exec::submitJob(player, std::move(job), error)) {
        // 提交失败时任务被销毁，finish 不会把记录放回去；canSubmit 已检查过，正常走不到这里。
        output.error(error);
        return false;
    }
    output.success(tr(undo ? "history.undo_started" : "history.redo_started"));
    return true;
}

void cancelPlayerJob(Player& player, Feedback const& output) {
    if (exec::cancelJob(player.getUuid())) output.success(tr("job.cancel_requested"));
    else output.error(tr("job.none"));
}

void clearClipboard(Player& player, Feedback const& output) {
    getSession(player).clipboard.reset();
    output.success(tr("clipboard.cleared"));
}

void clearHistory(Player& player, Feedback const& output) {
    getSession(player).history.clear();
    output.success(tr("history.cleared"));
}

void enableEditCommands() {
    registerListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto&) { registerCommands(); },
        ll::event::EventPriority::Low
    );
    if (ll::service::getCommandRegistry(false)) registerCommands();
}

void disableEditCommands() {
    if (registerListener) ll::event::EventBus::getInstance().removeListener(registerListener);
    registerListener.reset();
}

} // namespace creatorx
