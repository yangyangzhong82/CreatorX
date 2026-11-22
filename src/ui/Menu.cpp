#include "ui/Menu.h"
#include "ui/AdvancedMenu.h"

#include "command/EditCommands.h"
#include "command/AdvancedCommands.h"
#include "schematic/Schematic.h"
#include "command/SelectionCommands.h"
#include "edit/Pattern.h"
#include "exec/JobScheduler.h"
#include "mod/Global.h"
#include "session/PlayerSession.h"

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace creatorx::ui {
namespace {

using ll::form::CustomForm;
using ll::form::CustomFormResult;
using ll::form::FormCancelReason;
using ll::form::SimpleForm;

// 原版贴图路径，用作按钮图标。
namespace icon {
constexpr auto selection = "textures/items/wood_axe";
constexpr auto fill      = "textures/blocks/stone";
constexpr auto replace   = "textures/blocks/cobblestone";
constexpr auto clipboard = "textures/items/paper";
constexpr auto undo      = "textures/items/clock_item";
constexpr auto history   = "textures/items/book_writable";
constexpr auto cancel    = "textures/blocks/barrier";
constexpr auto settings  = "textures/items/redstone_dust";
constexpr auto help      = "textures/items/book_normal";
constexpr auto feet      = "textures/items/leather_boots";
constexpr auto look      = "textures/items/compass_item";
constexpr auto back      = "textures/items/arrow";
} // namespace icon

std::string text(std::string_view key) { return std::string(tr(key)); }

std::string_view trim(std::string_view value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}

std::string shortName(std::string const& name) {
    return name.starts_with("minecraft:") ? name.substr(10) : name;
}

// 在内容顶部显示上一步的结果；error 为真时标红。
std::string withNotice(std::string const& notice, bool error, std::string content) {
    if (notice.empty()) return content;
    return (error ? "§c" : "§a") + notice + "§r\n\n" + content;
}

void addButton(SimpleForm& form, std::string_view key, char const* image, SimpleForm::ButtonCallback callback) {
    form.appendButton(text(key), image, "path", std::move(callback));
}

// ---- 表单结果读取 ----

using Values = std::unordered_map<std::string, ll::form::CustomFormElementResult>;

double number(Values const& values, std::string const& key) {
    auto it = values.find(key);
    if (it == values.end()) return 0;
    if (auto* value = std::get_if<uint64>(&it->second)) return double(*value);
    if (auto* value = std::get_if<double>(&it->second)) return *value;
    return 0;
}

std::string inputText(Values const& values, std::string const& key) {
    auto it = values.find(key);
    if (it == values.end()) return {};
    if (auto* value = std::get_if<std::string>(&it->second)) return *value;
    return {};
}

size_t index(Values const& values, std::string const& key) { return size_t(number(values, key)); }

// ---- 状态摘要 ----

std::string selectionSummary(Player& player) {
    auto* session = findSession(player.getUuid());
    if (!session || session->dimension != int(player.getDimensionId())
        || (!session->cuboid.first && !session->cuboid.second)) {
        return text("menu.selection_none");
    }
    auto const& cuboid = session->cuboid;
    if (!cuboid.first || !cuboid.second) {
        auto const& known = cuboid.first ? *cuboid.first : *cuboid.second;
        return tr("menu.selection_partial", cuboid.first ? 1 : 2, known.x, known.y, known.z, cuboid.first ? 2 : 1);
    }
    auto a    = *cuboid.first;
    auto b    = *cuboid.second;
    auto size = *cuboid.size();
    return tr("menu.selection_info", a.x, a.y, a.z, b.x, b.y, b.z, size.x, size.y, size.z, size.volume());
}

std::string clipboardSummary(PlayerSession const& session) {
    if (!session.clipboard) return text("menu.clipboard_empty");
    auto size = session.clipboard->size;
    return tr("menu.clipboard_info", size.x, size.y, size.z);
}

std::string historySummary(PlayerSession const& session) {
    return tr("menu.history_info", session.history.undoCount(), session.history.redoCount());
}

bool hasSelection(Player& player) {
    auto* session = findSession(player.getUuid());
    return session && session->dimension == int(player.getDimensionId()) && session->cuboid.first
        && session->cuboid.second;
}

// ---- 图案来源下拉框 ----

struct Choice {
    std::string label;
    std::string value; // 空串表示使用输入框
};

struct Choices {
    std::vector<Choice> items;
    size_t              preferred = 0;

    std::vector<std::string> labels() const {
        std::vector<std::string> result;
        for (auto const& item : items) result.push_back(item.label);
        result.push_back(text("menu.source_search"));
        return result;
    }
    std::string value(size_t i) const { return i < items.size() ? items[i].value : std::string{}; }
    bool searches(size_t i) const { return i == items.size(); }
};

Choices patternChoices(Player& player, std::deque<std::string> const& recent, bool existingOption) {
    Choices choices;
    choices.items.push_back({text("menu.source_input"), {}});
    if (existingOption) {
        choices.items.push_back({text("menu.source_existing"), "#existing"});
        choices.preferred = 1;
    }
    auto const& item = player.getSelectedItem();
    if (!item.isNull() && item.mBlock) {
        if (!existingOption) choices.preferred = choices.items.size();
        choices.items.push_back({tr("menu.source_hand", shortName(item.mBlock->getTypeName())), "hand"});
    }
    for (auto const& entry : recent) {
        if (choices.preferred == 0) choices.preferred = choices.items.size();
        choices.items.push_back({tr("menu.source_recent", entry), entry});
    }
    return choices;
}

// 输入框优先，留空时用下拉框所选。
std::string pickPattern(Values const& values, std::string const& input, std::string const& source, Choices const& c) {
    auto raw   = inputText(values, input);
    auto typed = trim(raw);
    if (!typed.empty()) return std::string(typed);
    return c.value(index(values, source));
}

void openSelectionMenu(Player& player, std::string const& notice = {}, bool error = false);
void openFillForm(Player& player, size_t shape = 0, std::optional<size_t> source = {}, std::string input = {}, std::string error = {});
void openReplaceForm(Player& player, std::string mask = {}, std::string pattern = {}, std::string error = {});
void openClipboardMenu(Player& player, std::string const& notice = {}, bool error = false);
void openHistoryMenu(Player& player, std::string const& notice = {}, bool error = false);
void openSettingsMenu(Player& player);
void openHelp(Player& player);
void openMain(Player& player, std::string const& notice = {}, bool error = false);

// 执行一个会改变选区的操作后重新打开选区菜单，错误显示在菜单顶部。
template <class Action>
void selectionAction(Player& player, Action action) {
    std::string error;
    bool        ok = action(Feedback(player, &error));
    openSelectionMenu(player, ok ? std::string{} : error, !ok);
}

// ---- 选区 ----

std::optional<core::Vec3i> parseCoords(std::string_view value, BlockPos const& feet) {
    std::vector<std::string_view> parts;
    size_t                        start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == ' ' || value[i] == ',') {
            if (i > start) parts.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 3) return std::nullopt;
    std::array<int, 3> base{feet.x, feet.y, feet.z};
    std::array<int, 3> result{};
    for (size_t axis = 0; axis < 3; ++axis) {
        auto part     = parts[axis];
        bool relative = part.starts_with('~');
        if (relative) part.remove_prefix(1);
        int offset = 0;
        if (!part.empty()) {
            auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), offset);
            if (ec != std::errc{} || end != part.data() + part.size()) return std::nullopt;
        }
        result[axis] = relative ? base[axis] + offset : offset;
    }
    return core::Vec3i{result[0], result[1], result[2]};
}

std::string formatCoords(std::optional<core::Vec3i> const& pos) {
    return pos ? fmt::format("{} {} {}", pos->x, pos->y, pos->z) : std::string("~ ~ ~");
}

void openCoordsForm(Player& player, std::string first, std::string second, std::string error = {}) {
    CustomForm form(text("menu.coords_title"));
    form.appendLabel(error.empty() ? text("menu.coords_hint") : "§c" + error + "§r\n" + text("menu.coords_hint"));
    form.appendInput("first", text("menu.coords_first"), "x y z", first);
    form.appendInput("second", text("menu.coords_second"), "x y z", second);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openSelectionMenu(target);
        auto firstText  = inputText(*result, "first");
        auto secondText = inputText(*result, "second");
        auto feet       = target.getFeetBlockPos();
        auto first      = parseCoords(firstText, feet);
        auto second     = parseCoords(secondText, feet);
        if (!first || !second) {
            return openCoordsForm(target, firstText, secondText, text("menu.coords_invalid"));
        }
        std::string message;
        if (!setSelection(target, *first, *second, Feedback(target, &message))) {
            return openCoordsForm(target, firstText, secondText, message);
        }
        openSelectionMenu(target);
    });
}

void openResizeForm(Player& player, std::string error = {}) {
    static constexpr std::array directions{
        core::Direction::me,
        core::Direction::up,
        core::Direction::down,
        core::Direction::north,
        core::Direction::south,
        core::Direction::east,
        core::Direction::west
    };
    CustomForm form(text("menu.resize_title"));
    form.appendLabel(error.empty() ? selectionSummary(player) : "§c" + error + "§r\n" + selectionSummary(player));
    form.appendDropdown("mode", text("menu.resize_mode"), {text("menu.resize_expand"), text("menu.resize_contract")});
    form.appendDropdown(
        "direction",
        text("menu.resize_direction"),
        {text("menu.dir_me"),
         text("menu.dir_up"),
         text("menu.dir_down"),
         text("menu.dir_north"),
         text("menu.dir_south"),
         text("menu.dir_east"),
         text("menu.dir_west")}
    );
    form.appendSlider("amount", text("menu.resize_amount"), 1, 100, 1, 1);
    form.appendSlider("reverse", text("menu.resize_reverse"), 0, 100, 1, 0);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openSelectionMenu(target);
        auto        direction = directions[std::min(index(*result, "direction"), directions.size() - 1)];
        std::string message;
        bool        ok = resizeSelection(
            target,
            Feedback(target, &message),
            index(*result, "mode") == 1,
            int(std::lround(number(*result, "amount"))),
            int(std::lround(number(*result, "reverse"))),
            direction
        );
        if (!ok) return openResizeForm(target, message);
        openSelectionMenu(target);
    });
}

void openSelectionMenu(Player& player, std::string const& notice, bool error) {
    SimpleForm form(text("menu.selection_title"), withNotice(notice, error, selectionSummary(player) + "\n\n" + text("menu.selection_hint")));
    addButton(form, "menu.pos1_feet", icon::feet, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) {
            auto feet = p.getFeetBlockPos();
            return setCorner(p, true, {feet.x, feet.y, feet.z}, out);
        });
    });
    addButton(form, "menu.pos2_feet", icon::feet, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) {
            auto feet = p.getFeetBlockPos();
            return setCorner(p, false, {feet.x, feet.y, feet.z}, out);
        });
    });
    addButton(form, "menu.pos1_look", icon::look, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) { return setLookCorner(p, true, out); });
    });
    addButton(form, "menu.pos2_look", icon::look, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) { return setLookCorner(p, false, out); });
    });
    addButton(form, "menu.coords", icon::selection, [](Player& p) {
        auto& cuboid = getSession(p).cuboid;
        openCoordsForm(p, formatCoords(cuboid.first), formatCoords(cuboid.second));
    });
    addButton(form, "menu.resize", icon::selection, [](Player& p) {
        if (!hasSelection(p)) return openSelectionMenu(p, text("menu.need_selection"), true);
        openResizeForm(p);
    });
    addButton(form,"menu.adjust_selection",icon::selection,[](Player& p) {openAdjustMenu(p);});
    addButton(form, "menu.expand_vert", icon::selection, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) { return expandVertically(p, out); });
    });
    addButton(form, "menu.clear_selection", icon::cancel, [](Player& p) {
        selectionAction(p, [&](Feedback const& out) {
            clearSelection(p, out);
            return true;
        });
    });
    addButton(form, "menu.back", icon::back, [](Player& p) { openMain(p); });
    form.sendTo(player);
}

// ---- 填充 / 替换 ----

using BlockPicked = std::function<void(Player&, std::string)>;

void openBlockSearch(Player& player, BlockPicked picked, std::string query = {});

void openBlockResults(Player& player, BlockPicked picked, std::string query, size_t offset) {
    constexpr size_t pageSize = 24;
    auto ids = edit::registeredBlockIds();
    std::erase_if(ids, [&query](std::string const& id) { return id.find(query) == std::string::npos; });

    SimpleForm form(text("menu.block_search_title"));
    form.setContent(ids.empty() ? text("menu.block_no_results") : tr("menu.block_results", ids.size()));
    size_t end = std::min(offset + pageSize, ids.size());
    for (size_t i = offset; i < end; ++i) {
        form.appendButton(shortName(ids[i]), [picked, id = ids[i]](Player& p) { picked(p, id); });
    }
    if (offset != 0) {
        form.appendButton(text("menu.block_previous"), [picked, query, offset](Player& p) {
            openBlockResults(p, picked, query, offset - pageSize);
        });
    }
    if (end < ids.size()) {
        form.appendButton(text("menu.block_next"), [picked, query, end](Player& p) {
            openBlockResults(p, picked, query, end);
        });
    }
    form.appendButton(text("menu.block_search_again"), [picked, query](Player& p) { openBlockSearch(p, picked, query); });
    form.appendButton(text("menu.block_search_back"), [picked](Player& p) { picked(p, {}); });
    form.sendTo(player);
}

void openBlockSearch(Player& player, BlockPicked picked, std::string query) {
    CustomForm form(text("menu.block_search_title"));
    form.appendInput("query", text("menu.block_search_query"), text("menu.block_search_example"), query);
    form.setSubmitButton(text("menu.block_search_submit"));
    form.sendTo(player, [picked = std::move(picked)](Player& p, CustomFormResult const& result, FormCancelReason) {
        if (!result) return picked(p, {});
        auto input = inputText(*result, "query");
        auto query = std::string(trim(input));
        std::ranges::transform(query, query.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        openBlockResults(p, picked, std::move(query), 0);
    });
}

void openFillForm(Player& player, size_t shape, std::optional<size_t> source, std::string input, std::string error) {
    if (!hasSelection(player)) return openSelectionMenu(player, text("menu.need_selection"), true);
    auto choices = patternChoices(player, getSession(player).recentPatterns, false);

    CustomForm form(text("menu.fill_title"));
    auto       label = selectionSummary(player) + "\n" + text("menu.pattern_hint");
    form.appendLabel(error.empty() ? label : "§c" + error + "§r\n" + label);
    form.appendDropdown(
        "shape",
        text("menu.shape"),
        {text("menu.shape_all"), text("menu.shape_walls"), text("menu.shape_faces")},
        std::min<size_t>(shape, 2)
    );
    form.appendDropdown(
        "source",
        text("menu.pattern_source"),
        choices.labels(),
        std::min(source.value_or(choices.preferred), choices.items.size() - 1)
    );
    form.appendInput("pattern", text("menu.pattern_input"), text("menu.pattern_placeholder"), input);
    form.setSubmitButton(text("menu.fill_submit"));
    form.sendTo(player, [choices](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return;
        static constexpr std::array shapes{core::BoxShape::all, core::BoxShape::walls, core::BoxShape::faces};
        auto shapeIndex  = std::min(index(*result, "shape"), shapes.size() - 1);
        auto sourceIndex = index(*result, "source");
        auto typed       = inputText(*result, "pattern");
        if (choices.searches(sourceIndex)) {
            return openBlockSearch(target, [shapeIndex, typed](Player& p, std::string id) {
                openFillForm(p, shapeIndex, 0, id.empty() ? typed : std::move(id));
            });
        }
        auto pattern = pickPattern(*result, "pattern", "source", choices);
        if (pattern.empty()) return openFillForm(target, shapeIndex, sourceIndex, typed, text("menu.need_pattern"));
        std::string message;
        if (!regionEdit(target, Feedback(target, &message), shapes[shapeIndex], pattern, std::nullopt)) {
            // 把实际尝试的图案放进输入框，玩家可以直接改。
            openFillForm(target, shapeIndex, 0, pattern, message);
        }
    });
}

void openReplaceForm(Player& player, std::string mask, std::string pattern, std::string error) {
    if (!hasSelection(player)) return openSelectionMenu(player, text("menu.need_selection"), true);
    auto& session = getSession(player);
    auto  from    = patternChoices(player, session.recentMasks, true);
    auto  to      = patternChoices(player, session.recentPatterns, false);

    CustomForm form(text("menu.replace_title"));
    auto       label = selectionSummary(player) + "\n" + text("menu.replace_hint");
    form.appendLabel(error.empty() ? label : "§c" + error + "§r\n" + label);
    // 重新打开时上次的内容放在输入框里，下拉框指向“使用下方输入框”。
    form.appendDropdown("from_source", text("menu.replace_from"), from.labels(), mask.empty() ? from.preferred : 0);
    form.appendInput("from", text("menu.mask_input"), text("menu.mask_placeholder"), mask);
    form.appendDropdown("to_source", text("menu.replace_to"), to.labels(), pattern.empty() ? to.preferred : 0);
    form.appendInput("to", text("menu.pattern_input"), text("menu.pattern_placeholder"), pattern);
    form.setSubmitButton(text("menu.replace_submit"));
    form.sendTo(player, [from, to](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return;
        auto mask    = pickPattern(*result, "from", "from_source", from);
        auto pattern = pickPattern(*result, "to", "to_source", to);
        bool searchFrom = from.searches(index(*result, "from_source"));
        bool searchTo   = to.searches(index(*result, "to_source"));
        if (searchFrom || searchTo) {
            return openBlockSearch(target, [mask, pattern, searchFrom, searchTo](Player& p, std::string id) {
                if (searchFrom) {
                    if (searchTo && !id.empty()) {
                        return openBlockSearch(p, [selectedMask = std::move(id), pattern](Player& next, std::string block) {
                            openReplaceForm(next, selectedMask, block.empty() ? pattern : std::move(block));
                        });
                    }
                    openReplaceForm(p, id.empty() ? mask : std::move(id), pattern);
                } else {
                    openReplaceForm(p, mask, id.empty() ? pattern : std::move(id));
                }
            });
        }
        if (mask.empty()) return openReplaceForm(target, mask, pattern, text("menu.need_mask"));
        if (pattern.empty()) return openReplaceForm(target, mask, pattern, text("menu.need_pattern"));
        std::string message;
        if (!regionEdit(target, Feedback(target, &message), core::BoxShape::all, pattern, std::string_view(mask))) {
            openReplaceForm(target, mask, pattern, message);
        }
    });
}

// ---- 剪贴板 ----

void openPasteForm(Player& player) {
    CustomForm form(text("menu.paste_title"));
    form.appendLabel(clipboardSummary(getSession(player)) + "\n" + text("menu.paste_hint"));
    form.appendToggle("skipAir", text("menu.paste_skip_air"), true);
    form.appendToggle("original", text("menu.paste_original"), false);
    form.appendToggle("select", text("menu.paste_select"), true);
    form.appendToggle("preview", text("menu.paste_preview"), true);
    form.appendToggle("entities",text("menu.copy_entities"),false);
    form.appendInput("mask",text("menu.mask_input"),"#existing","");
    form.setSubmitButton(text("menu.paste_submit"));
    form.sendTo(player, [](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openClipboardMenu(target);
        PasteOptions options{
            number(*result, "skipAir") != 0,
            number(*result, "original") != 0,
            number(*result, "select") != 0,
            number(*result, "preview") != 0,
            number(*result,"entities")!=0
        };
        auto mask=inputText(*result,"mask");
        if(!mask.empty()) options.mask=std::move(mask);
        std::string message;
        if (!pasteClipboard(target, Feedback(target, &message), options)) openClipboardMenu(target, message, true);
    });
}

void openClipboardMenu(Player& player, std::string const& notice, bool error) {
    auto& session = getSession(player);
    SimpleForm form(text("menu.clipboard_title"), withNotice(notice, error, clipboardSummary(session) + "\n\n" + text("menu.clipboard_hint")));
    auto run = [](Player& p, bool cut) {
        std::string message;
        if (!copySelection(p, Feedback(p, &message), cut, std::nullopt)) openClipboardMenu(p, message, true);
    };
    addButton(form, "menu.copy", icon::clipboard, [run](Player& p) { run(p, false); });
    addButton(form,"menu.copy_options",icon::clipboard,[](Player& p) {openCopyOptions(p);});
    addButton(form, "menu.cut", icon::clipboard, [run](Player& p) { run(p, true); });
    if (session.clipboard) {
        addButton(form, "menu.paste", icon::clipboard, [](Player& p) { openPasteForm(p); });
    addButton(form,"menu.transform",icon::clipboard,[](Player& p) {openTransformMenu(p);});
    addButton(form,"menu.schematics",icon::clipboard,[](Player& p) {openSchematicMenu(p);});
        addButton(form, "menu.clear_clipboard", icon::cancel, [](Player& p) {
            clearClipboard(p, Feedback(p));
            openClipboardMenu(p);
        });
    }
    addButton(form, "menu.back", icon::back, [](Player& p) { openMain(p); });
    form.sendTo(player);
}

// ---- 历史 ----

void openHistoryCountForm(Player& player, bool undo) {
    auto& history = getSession(player).history;
    auto  limit   = undo ? history.undoCount() : history.redoCount();
    if (limit < 2) return openHistoryMenu(player);
    CustomForm form(text(undo ? "menu.undo_many_title" : "menu.redo_many_title"));
    form.appendSlider("times", text("menu.history_times"), 1, double(limit), 1, 1);
    form.setSubmitButton(text(undo ? "menu.undo_submit" : "menu.redo_submit"));
    form.sendTo(player, [undo](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openHistoryMenu(target);
        std::string message;
        if (!runHistory(target, Feedback(target, &message), undo, int(std::lround(number(*result, "times"))))) {
            openHistoryMenu(target, message, true);
        }
    });
}

void historyStep(Player& player, bool undo) {
    std::string message;
    if (!runHistory(player, Feedback(player, &message), undo, 1)) openHistoryMenu(player, message, true);
}

void openHistoryMenu(Player& player, std::string const& notice, bool error) {
    auto& history = getSession(player).history;
    SimpleForm form(text("menu.history_title"), withNotice(notice, error, getSession(player).history.describe()));
    if (history.undoCount()) addButton(form, "menu.undo_one", icon::undo, [](Player& p) { historyStep(p, true); });
    if (history.redoCount()) addButton(form, "menu.redo_one", icon::undo, [](Player& p) { historyStep(p, false); });
    if (history.undoCount() > 1) {
        addButton(form, "menu.undo_many", icon::history, [](Player& p) { openHistoryCountForm(p, true); });
    }
    if (history.redoCount() > 1) {
        addButton(form, "menu.redo_many", icon::history, [](Player& p) { openHistoryCountForm(p, false); });
    }
    if (history.undoCount() || history.redoCount()) {
        addButton(form, "menu.clear_history", icon::cancel, [](Player& p) {
            ll::form::ModalForm confirm(
                text("menu.clear_history"),
                text("menu.clear_history_confirm"),
                text("menu.confirm_yes"),
                text("menu.confirm_no")
            );
            confirm.sendTo(p, [](Player& target, ll::form::ModalFormResult result, FormCancelReason) {
                if (result == ll::form::ModalFormSelectedButton::Upper) clearHistory(target, Feedback(target));
                openHistoryMenu(target);
            });
        });
    }
    addButton(form, "menu.back", icon::back, [](Player& p) { openMain(p); });
    form.sendTo(player);
}

// ---- 设置与帮助 ----

void openSettingsMenu(Player& player) {
    auto& session = getSession(player);
    SimpleForm form(text("menu.settings_title"), text("menu.settings_hint"));
    addButton(form, "menu.get_wand", icon::selection, [](Player& p) {
        giveWand(p, Feedback(p));
        openSettingsMenu(p);
    });
    addButton(form,"menu.reload",icon::settings,[](Player& p) {reloadConfiguration(Feedback(p));});
    form.appendButton(
        text(session.wandEnabled ? "menu.wand_on" : "menu.wand_off"),
        icon::selection,
        "path",
        [](Player& p) {
            toggleWand(p, Feedback(p));
            openSettingsMenu(p);
        }
    );
    form.appendButton(
        text(session.showOutline ? "menu.outline_on" : "menu.outline_off"),
        icon::look,
        "path",
        [](Player& p) {
            toggleOutline(p, Feedback(p));
            openSettingsMenu(p);
        }
    );
    addButton(form, "menu.back", icon::back, [](Player& p) { openMain(p); });
    form.sendTo(player);
}

void openHelp(Player& player) {
    SimpleForm form(text("menu.help_title"), text("menu.help_content"));
    addButton(form, "menu.back", icon::back, [](Player& p) { openMain(p); });
    form.sendTo(player);
}

// ---- 主菜单 ----

void openMain(Player& player, std::string const& notice, bool error) {
    auto& session = getSession(player);
    auto  status  = selectionSummary(player) + "\n" + clipboardSummary(session) + "\n" + historySummary(session);
    if (auto percent = exec::jobPercent(player.getUuid())) status += "\n" + tr("menu.job_running", *percent);

    SimpleForm form(text("menu.title"), withNotice(notice, error, status));
    addButton(form, "menu.selection", icon::selection, [](Player& p) { openSelectionMenu(p); });
    addButton(form, "menu.fill", icon::fill, [](Player& p) { openFillForm(p); });
    addButton(form,"menu.build",icon::fill,[](Player& p) {openBuildMenu(p);});
    addButton(form, "menu.replace", icon::replace, [](Player& p) { openReplaceForm(p); });
    addButton(form, "menu.clipboard", icon::clipboard, [](Player& p) { openClipboardMenu(p); });
    if (session.history.undoCount()) {
        addButton(form, "menu.undo_last", icon::undo, [](Player& p) {
            std::string message;
            if (!runHistory(p, Feedback(p, &message), true, 1)) openMain(p, message, true);
        });
    }
    addButton(form, "menu.history", icon::history, [](Player& p) { openHistoryMenu(p); });
    if (exec::hasJob(player.getUuid())) {
        addButton(form, "menu.cancel_job", icon::cancel, [](Player& p) { cancelPlayerJob(p, Feedback(p)); });
    }
    addButton(form, "menu.settings", icon::settings, [](Player& p) { openSettingsMenu(p); });
    addButton(form, "menu.help", icon::help, [](Player& p) { openHelp(p); });
    form.sendTo(player);
}

} // namespace

void openMainMenu(Player& player) {
    if (player.getCommandPermissionLevel() < CommandPermissionLevel::GameDirectors) {
        player.sendMessage(tr("command.no_permission"));
        return;
    }
    openMain(player);
}

} // namespace creatorx::ui
