#include "ui/Menu.h"
#include "ui/AdvancedMenu.h"
#include "ui/BlockBrowser.h"

#include "command/EditCommands.h"
#include "command/AdvancedCommands.h"
#include "schematic/Schematic.h"
#include "command/SelectionCommands.h"
#include "edit/Pattern.h"
#include "exec/JobScheduler.h"
#include "input/BrushTools.h"
#include "mod/Global.h"
#include "net/Enhancements.h"
#include "session/PlayerSession.h"
#include "visual/PastePreview.h"

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
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
constexpr auto brush     = "textures/items/blaze_rod";
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
    if(!cuboid.complete()) return std::string(tr("selection.need_vertices"));
    auto bounds=*cuboid.box();
    auto a    = bounds.min;
    auto b    = bounds.max;
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

    size_t selectedIndex(Values const& values, std::string const& key) const {
        // Dropdown 回调返回选项文字，需要映射回发送时的选项序号。
        auto selected = inputText(values, key);
        if (selected == text("menu.source_search")) return items.size();
        auto it = std::find_if(items.begin(), items.end(), [&selected](Choice const& choice) {
            return choice.label == selected;
        });
        return it == items.end() ? 0 : size_t(it - items.begin());
    }
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
    return c.value(c.selectedIndex(values, source));
}

void openSelectionMenu(Player& player, std::string const& notice = {}, bool error = false);
void openFillForm(Player& player, size_t shape = 0, std::optional<size_t> source = {}, std::string input = {}, std::string error = {});
void openReplaceForm(Player& player, std::string mask = {}, std::string pattern = {}, std::string error = {});
void openClipboardMenu(Player& player, std::string const& notice = {}, bool error = false);
void openHistoryMenu(Player& player, std::string const& notice = {}, bool error = false);
void openSettingsMenu(Player& player);
void openBrushMenu(Player& player);
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

std::optional<core::Vec3i> parseCoords(std::string_view value, Player& player) {
    std::vector<std::string_view> parts;
    size_t                        start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == ' ' || value[i] == '\t' || value[i] == ',') {
            if (i > start) parts.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 3) return std::nullopt;
    if (parts.front().starts_with('^')) {
        std::array<float, 3> offsets{};
        for (size_t axis = 0; axis < offsets.size(); ++axis) {
            auto part = parts[axis];
            if (!part.starts_with('^')) return std::nullopt;
            part.remove_prefix(1);
            if (part.empty()) continue;
            auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), offsets[axis]);
            if (ec != std::errc{} || end != part.data() + part.size()) return std::nullopt;
        }
        CommandPosition position{};
        position.mOffset = Vec3{offsets[0], offsets[1], offsets[2]};
        position.mLocal = position.mRelativeX = position.mRelativeY = position.mRelativeZ = true;
        PlayerCommandOrigin origin(player.getLevel(), player.getOrCreateUniqueID());
        auto world = position.getPosition(static_cast<int>(CurrentCmdVersion::Latest), origin, Vec3{0, 0, 0}) + 0.001f;
        for (double coordinate : {world.x, world.y, world.z}) {
            if (!(coordinate >= std::numeric_limits<int>::min() && coordinate <= std::numeric_limits<int>::max())) {
                return std::nullopt;
            }
        }
        BlockPos block = world;
        return core::Vec3i{block.x, block.y, block.z};
    }
    auto feet = player.getFeetBlockPos();
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
        auto coordinate = std::int64_t(offset) + (relative ? base[axis] : 0);
        if (!std::in_range<int>(coordinate)) return std::nullopt;
        result[axis] = int(coordinate);
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
        auto first      = parseCoords(firstText, target);
        auto second     = parseCoords(secondText, target);
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

constexpr std::array<std::string_view, 13> buildOperations{
    "sphere", "hsphere", "cyl", "hcyl", "pyramid", "hpyramid", "hollow", "overlay", "center", "move", "stack", "count", "distr"
};

void openBuildOperation(
    Player& player, std::string_view operation, Values const& saved = {}, std::string const& error = {}
) {
    bool sphere = operation == "sphere" || operation == "hsphere";
    bool cylinder = operation == "cyl" || operation == "hcyl";
    bool pyramid = operation == "pyramid" || operation == "hpyramid";
    bool transfer = operation == "move" || operation == "stack";
    bool hollow = operation == "hollow";
    bool distribution = operation == "distr";
    bool count = operation == "count";
    bool usesPattern = !transfer && !distribution;
    static constexpr std::array directions{"", "up", "down", "north", "south", "east", "west"};

    auto& session = getSession(player);
    auto choices = usesPattern ? patternChoices(player, count ? session.recentMasks : session.recentPatterns, count)
                               : Choices{};
    CustomForm form(text("operation." + std::string(operation)));
    if (!error.empty()) form.appendLabel("§c" + error + "§r");
    if (usesPattern) {
        auto typed = inputText(saved, "pattern");
        auto preferred = typed.empty() && !hollow ? choices.preferred : 0;
        form.appendDropdown("source", text(count ? "menu.replace_from" : "menu.pattern_source"), choices.labels(), preferred);
        form.appendInput(
            "pattern", text(count ? "menu.mask_input" : "menu.pattern_input"),
            text(count ? "menu.mask_placeholder" : "menu.pattern_placeholder"), typed
        );
    }
    if (sphere || cylinder || pyramid || transfer || hollow) {
        auto key = sphere || cylinder ? "menu.build_radius" : pyramid ? "menu.build_layers"
                   : hollow ? "menu.build_thickness" : operation == "move" ? "menu.build_distance" : "menu.build_copies";
        auto initial = hollow || transfer ? "1" : "5";
        form.appendInput("amount", text(key), initial, saved.contains("amount") ? inputText(saved, "amount") : initial);
    }
    if (cylinder) form.appendInput("height", text("menu.height"), "10", saved.contains("height") ? inputText(saved, "height") : "10");
    if (sphere) form.appendToggle("raised", text("menu.build_raised"), number(saved, "raised") != 0);
    if (sphere || cylinder || pyramid) {
        form.appendToggle("center", text("menu.build_selection_center"), number(saved, "center") != 0);
        form.appendInput("mask", text("menu.build_mask"), "#air", inputText(saved, "mask"));
    }
    if (transfer) {
        form.appendDropdown("direction", text("menu.resize_direction"),
            {text("menu.dir_me"), text("menu.dir_up"), text("menu.dir_down"), text("menu.dir_north"),
             text("menu.dir_south"), text("menu.dir_east"), text("menu.dir_west")}, index(saved, "direction"));
        form.appendToggle("air", text("menu.paste_skip_air"), saved.empty() || number(saved, "air") != 0);
        form.appendToggle("select", text("menu.build_follow"), saved.empty() || number(saved, "select") != 0);
        form.appendToggle("entities", text("menu.copy_entities"), number(saved, "entities") != 0);
        form.appendInput("mask", text("menu.build_mask"), "#existing", inputText(saved, "mask"));
    }
    if (distribution) form.appendToggle("clipboard", text("menu.build_distr_clipboard"), number(saved, "clipboard") != 0);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [operation, choices, usesPattern, sphere, cylinder, pyramid, transfer, hollow, distribution](
        Player& target, CustomFormResult const& result, FormCancelReason
    ) {
        if (!result) return openBuildMenu(target);
        if (usesPattern && choices.searches(choices.selectedIndex(*result, "source"))) {
            return openBlockBrowser(target, [operation, saved = *result](Player& p, std::string id) mutable {
                if (!id.empty()) saved["pattern"] = std::move(id);
                openBuildOperation(p, operation, saved);
            });
        }
        auto pattern = usesPattern ? pickPattern(*result, "pattern", "source", choices) : std::string{};
        auto amount = inputText(*result, "amount");
        auto args = pattern;
        if (hollow) args = amount + (pattern.empty() ? "" : " " + pattern);
        if (sphere || cylinder || pyramid) {
            if (pattern.empty()) return openBuildOperation(target, operation, *result, text("menu.need_pattern"));
            args = pattern + " " + amount;
            if (cylinder) args += " " + inputText(*result, "height");
            if (sphere && number(*result, "raised")) args += " -r";
            if (number(*result, "center")) args += " -c";
            auto mask = inputText(*result, "mask");
            if (!trim(mask).empty()) args += " -m " + mask;
        }
        if (transfer) {
            args = amount + " " + directions[std::min(index(*result, "direction"), directions.size() - 1)];
            if (number(*result, "air")) args += " -a";
            if (number(*result, "select")) args += " -s";
            if (number(*result, "entities")) args += " -e";
            auto mask = inputText(*result, "mask");
            if (!trim(mask).empty()) args += " -m " + mask;
        }
        if (distribution && number(*result, "clipboard")) args = "-c";
        std::string message;
        executeAdvanced(target, Feedback(target, &message), operation, args);
        if (!message.empty()) {
            auto retry = *result;
            retry["pattern"] = std::move(pattern);
            openBuildOperation(target, operation, retry, message);
        }
    });
}

void openFillForm(Player& player, size_t shape, std::optional<size_t> source, std::string input, std::string error) {
    if (!hasSelection(player)) return openSelectionMenu(player, text("menu.need_selection"), true);
    (void)source;
    openFillDdui(player, shape, std::move(input), std::move(error), [](Player& target, size_t shapeIndex, std::string pattern) {
        static constexpr std::array shapes{core::BoxShape::all, core::BoxShape::walls, core::BoxShape::faces};
        if (pattern.empty()) return openFillForm(target, shapeIndex, {}, {}, text("menu.need_pattern"));
        std::string message;
        if (!regionEdit(target, Feedback(target, &message), shapes[std::min<size_t>(shapeIndex, 2)], pattern, std::nullopt))
            openFillForm(target, shapeIndex, {}, std::move(pattern), std::move(message));
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
        if (!result) return openMain(target);
        auto mask    = pickPattern(*result, "from", "from_source", from);
        auto pattern = pickPattern(*result, "to", "to_source", to);
        bool searchFrom = from.searches(from.selectedIndex(*result, "from_source"));
        bool searchTo   = to.searches(to.selectedIndex(*result, "to_source"));
        if (searchFrom || searchTo) {
            return openBlockBrowser(target, [mask, pattern, searchFrom, searchTo](Player& p, std::string id) {
                if (searchFrom) {
                    if (searchTo && !id.empty()) {
                        return openBlockBrowser(p, [selectedMask = std::move(id), pattern](Player& next, std::string block) {
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

void openBrushMenu(Player& player) {
    static constexpr std::array<std::string_view, 8> actions{
        "sphere", "cyl", "smooth", "clipboard", "gravity", "info", "repl", "none"
    };
    auto& session = getSession(player);
    auto const& item = player.getSelectedItem();
    BrushBinding const* current = nullptr;
    if (!item.isNull()) {
        auto it = session.brushBindings.find(item.getTypeName());
        if (it != session.brushBindings.end()) current = &it->second;
    }
    CustomForm form(text("menu.brush_title"));
    form.appendLabel(item.isNull() ? text("menu.brush_no_item") : tr("menu.brush_item", item.getTypeName()));
    std::vector<std::string> labels;
    for (auto action : actions) labels.push_back(text("menu.brush_" + std::string(action)));
    size_t selected = current ? static_cast<size_t>(current->tool) : 0;
    if (!current && item.isNull()) selected = 7;
    form.appendDropdown("type", text("menu.brush_type"), labels, std::min(selected, actions.size() - 1));
    form.appendInput("pattern", text("menu.brush_pattern"), "stone", current ? current->patternText : "");
    form.appendInput("radius", text("menu.brush_radius"), "3", current ? std::to_string(current->radius) : "3");
    form.appendInput("height", text("menu.brush_height"), "1", current ? std::to_string(current->height) : "1");
    form.appendInput("iterations", text("menu.brush_iterations"), "1", current ? std::to_string(current->iterations) : "1");
    form.appendInput("range", text("menu.brush_range"), "32", current ? std::to_string(current->range) : "32");
    form.appendInput("mask", text("menu.brush_mask"), "#existing", current ? current->maskText : "");
    form.appendToggle("hollow", text("menu.brush_hollow"), current && current->hollow);
    form.appendToggle("skip_air", text("menu.paste_skip_air"), current && current->skipAir);
    form.appendToggle("original", text("menu.paste_original"), current && current->original);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openMain(target);
        auto action = actions[std::min(index(*result, "type"), actions.size() - 1)];
        if (action == "none") {
            executeBrushOption(target, Feedback(target), "none", {});
            return openBrushMenu(target);
        }
        auto pattern = inputText(*result, "pattern");
        auto radius = inputText(*result, "radius");
        auto height = inputText(*result, "height");
        auto iterations = inputText(*result, "iterations");
        std::string arguments;
        if (action == "sphere" || action == "cyl") {
            if (pattern.empty()) return openBrushMenu(target);
            arguments = pattern + " " + radius;
            if (action == "cyl") arguments += " " + height;
            if (number(*result, "hollow")) arguments += " -h";
        } else if (action == "smooth") {
            arguments = radius + " " + iterations;
        } else if (action == "gravity") {
            arguments = radius;
        } else if (action == "clipboard") {
            if (number(*result, "skip_air")) arguments += " -a";
            if (number(*result, "original")) arguments += " -o";
        } else if (action == "repl") {
            arguments = pattern;
        }
        if (action == "info" || action == "repl") executeTool(target, Feedback(target), action, arguments);
        else executeBrush(target, Feedback(target), action, arguments);
        auto range = inputText(*result, "range");
        if (range != "32" && !range.empty()) executeBrushOption(target, Feedback(target), "range", range);
        auto mask = inputText(*result, "mask");
        if (!mask.empty() && action != "info" && action != "repl") executeBrushOption(target, Feedback(target), "mask", mask);
        openBrushMenu(target);
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

void openPreviewAdjustForm(Player& player) {
    CustomForm form(text("menu.preview_adjust"));
    form.appendInput("x", text("menu.preview_x"), "0", "0");
    form.appendInput("y", text("menu.preview_y"), "0", "0");
    form.appendInput("z", text("menu.preview_z"), "0", "0");
    form.appendDropdown("rotation", text("menu.rotation_y"), {"0", "90", "180", "270"});
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [](Player& target, CustomFormResult const& result, FormCancelReason) {
        if (!result) return openClipboardMenu(target);
        core::Vec3i offset{};
        std::array<int*, 3> values{&offset.x, &offset.y, &offset.z};
        constexpr std::array keys{"x", "y", "z"};
        for (size_t i = 0; i < keys.size(); ++i) {
            auto raw = inputText(*result, keys[i]);
            auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), *values[i]);
            if (ec != std::errc{} || end != raw.data() + raw.size()) {
                return openClipboardMenu(target, text("preview.bad_offset"), true);
            }
        }
        auto rotation = 90 * int(std::min(index(*result, "rotation"), size_t{3}));
        std::string message;
        if (!adjustPastePreview(target, Feedback(target, &message), offset, rotation)) {
            openClipboardMenu(target, message, true);
        }
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
    addButton(form, "menu.schematics", icon::clipboard, [](Player& p) { openSchematicMenu(p); });
    if (getPastePreview(player)) {
        addButton(form, "menu.preview_confirm", icon::clipboard, [](Player& p) {
            std::string message;
            if (!confirmPastePreview(p, Feedback(p, &message))) openClipboardMenu(p, message, true);
        });
        addButton(form, "menu.preview_adjust", icon::settings, [](Player& p) { openPreviewAdjustForm(p); });
        addButton(form, "menu.preview_cancel", icon::cancel, [](Player& p) {
            if (cancelPastePreview(p)) p.sendMessage(tr("preview.cancelled"));
            openClipboardMenu(p);
        });
    }
    if (session.clipboard) {
        addButton(form, "menu.paste", icon::clipboard, [](Player& p) { openPasteForm(p); });
        addButton(form,"menu.transform",icon::clipboard,[](Player& p) {openTransformMenu(p);});
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
    addButton(form, "menu.hud_settings", icon::settings, [](Player& p) {
        auto& preferences = getSession(p);
        CustomForm hudForm(text("menu.hud_settings"));
        hudForm.appendToggle("show", text("menu.hud_show"), preferences.showHud);
        hudForm.appendSlider(
            "idle", text("menu.hud_idle_seconds"), minHudIdleSeconds, maxHudIdleSeconds, 1, preferences.hudIdleSeconds
        );
        hudForm.setSubmitButton(text("menu.apply"));
        hudForm.sendTo(p, [](Player& target, CustomFormResult const& result, FormCancelReason) {
            if (result) {
                setHudPreferences(target, number(*result, "show") != 0, int(std::lround(number(*result, "idle"))));
            }
            openSettingsMenu(target);
        });
    });
    addButton(form, session.remoteWand ? "menu.remote_wand_on" : "menu.remote_wand_off", icon::look, [](Player& p) {
        auto& preferences = getSession(p);
        preferences.remoteWand = !preferences.remoteWand;
        saveSession(p.getUuid());
        openSettingsMenu(p);
    });
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
    noteHudActivity(player);
    auto& session = getSession(player);
    auto  status  = selectionSummary(player) + "\n" + clipboardSummary(session) + "\n" + historySummary(session);
    if (auto percent = exec::jobPercent(player.getUuid())) status += "\n" + tr("menu.job_running", *percent);

    SimpleForm form(text("menu.title"), withNotice(notice, error, status));
    addButton(form, "menu.selection", icon::selection, [](Player& p) { openSelectionMenu(p); });
    if (session.wandEnabled && session.remoteWand) {
        addButton(form, "menu.pos1_look", icon::look, [](Player& p) { setLookCorner(p, true, Feedback(p)); });
        addButton(form, "menu.pos2_look", icon::look, [](Player& p) { setLookCorner(p, false, Feedback(p)); });
    }
    addButton(form, "menu.fill", icon::fill, [](Player& p) { openFillForm(p); });
    addButton(form,"menu.build",icon::fill,[](Player& p) {openBuildMenu(p);});
    addButton(form, "menu.replace", icon::replace, [](Player& p) { openReplaceForm(p); });
    addButton(form, "menu.clipboard", icon::clipboard, [](Player& p) { openClipboardMenu(p); });
    addButton(form, "menu.brushes", icon::brush, [](Player& p) { openBrushMenu(p); });
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

void openBuildMenu(Player& player) {
    SimpleForm form(text("menu.build"));
    for (auto operation : buildOperations) form.appendButton(text("operation." + std::string(operation)));
    form.appendButton(text("menu.back"));
    form.sendTo(player, [](Player& target, int selected, FormCancelReason) {
        if (selected < 0 || size_t(selected) >= buildOperations.size()) return openMainMenu(target);
        openBuildOperation(target, buildOperations[size_t(selected)]);
    });
}

void openClipboardRoot(Player& player) { openClipboardMenu(player); }
void openSelectionRoot(Player& player) { openSelectionMenu(player); }

void openMainMenu(Player& player) {
    if (player.getCommandPermissionLevel() < CommandPermissionLevel::GameDirectors) {
        player.sendMessage(tr("command.no_permission"));
        return;
    }
    openMain(player);
}

} // namespace creatorx::ui
