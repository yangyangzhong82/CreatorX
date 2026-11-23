#include "ui/AdvancedMenu.h"
#include "ui/Menu.h"
#include "command/AdvancedCommands.h"
#include "command/EditCommands.h"
#include "command/SelectionCommands.h"
#include "mod/Global.h"
#include "schematic/Schematic.h"
#include "session/PlayerSession.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/world/actor/player/Player.h"
#include <charconv>
#include <array>
#include <algorithm>
#include <cmath>

namespace creatorx::ui {
namespace {
std::string text(std::string_view key) {return std::string(tr(key));}
using Values=std::unordered_map<std::string,ll::form::CustomFormElementResult>;
std::string input(Values const& values,std::string const& key) {
    if(auto it=values.find(key);it!=values.end()) if(auto value=std::get_if<std::string>(&it->second)) return *value;
    return {};
}
size_t number(Values const& values,std::string const& key) {
    if(auto it=values.find(key);it!=values.end()) {
        if(auto value=std::get_if<uint64>(&it->second)) return *value;
        if(auto value=std::get_if<double>(&it->second)) return size_t(*value);
    }
    return 0;
}
} // namespace

void openTransformMenu(Player& player) {
    ll::form::CustomForm form(text("menu.transform"));
    form.appendDropdown("mode",text("menu.action"),{text("menu.rotate"),text("menu.flip")});
    form.appendInput("y",text("menu.rotation_y"),"90","90");
    form.appendInput("x",text("menu.rotation_x"),"0","0");
    form.appendInput("z",text("menu.rotation_z"),"0","0");
    form.appendDropdown("axis",text("menu.resize_direction"),{text("menu.dir_me"),text("menu.dir_east"),text("menu.dir_up"),text("menu.dir_south")});
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openClipboardRoot(target);
        if(number(*result,"mode")==1) {
            static constexpr std::array axes{core::Direction::me,core::Direction::east,core::Direction::up,core::Direction::south};
            flipClipboard(target,Feedback(target),axes[std::min(number(*result,"axis"),axes.size()-1)]);
        } else {
            std::array<int,3> values{};
            std::array<std::string,3> raw{input(*result,"y"),input(*result,"x"),input(*result,"z")};
            for(size_t i=0;i<3;++i) {
                auto [end,ec]=std::from_chars(raw[i].data(),raw[i].data()+raw[i].size(),values[i]);
                if(ec!=std::errc{} || end!=raw[i].data()+raw[i].size()) {target.sendMessage(tr("clipboard.rotation_invalid"));return;}
            }
            rotateClipboard(target,Feedback(target),values[0],values[1],values[2]);
        }
    });
}
namespace {
void openSchematicBrowser(Player& player, std::string filter = {}, size_t page = 0);
void openSchematicFile(Player& player, std::string name, std::string filter, size_t page);

void openSchematicSearch(Player& player, std::string filter) {
    ll::form::CustomForm form(text("menu.schem_search"));
    form.appendInput("filter", text("menu.schem_name"), "", filter);
    form.sendTo(player, [filter = std::move(filter)](
        Player& target, ll::form::CustomFormResult const& result, ll::form::FormCancelReason
    ) {
        openSchematicBrowser(target, result ? input(*result, "filter") : filter);
    });
}

void openSchematicPlace(Player& player, std::string name, std::string filter, size_t page, std::string error = {}) {
    ll::form::CustomForm form(text("menu.schem_place"));
    if (!error.empty()) form.appendLabel("§c" + error + "§r");
    form.appendToggle("skip_air", text("menu.paste_skip_air"), true);
    form.appendToggle("entities", text("menu.copy_entities"), false);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player, [name = std::move(name), filter = std::move(filter), page](
        Player& target, ll::form::CustomFormResult const& result, ll::form::FormCancelReason
    ) {
        if (!result) return openSchematicFile(target, name, filter, page);
        std::string message;
        if (!executeSchematic(
                target, Feedback(target, &message), "place", name, "mcstructure",
                number(*result, "skip_air") != 0, number(*result, "entities") != 0
            )) {
            return openSchematicPlace(target, name, filter, page, message);
        }
        openSchematicBrowser(target, filter, page);
    });
}

void openSchematicFile(Player& player, std::string name, std::string filter, size_t page) {
    ll::form::SimpleForm form(name);
    form.appendButton(text("menu.schem_load"));
    form.appendButton(text("menu.schem_place"));
    form.appendButton(text("menu.schem_delete"));
    form.appendButton(text("menu.schem_browse"));
    form.sendTo(player, [name = std::move(name), filter = std::move(filter), page](
        Player& target, int selected, ll::form::FormCancelReason
    ) {
        if (selected == 0) {
            if (executeSchematic(target, Feedback(target), "load", name)) return;
            return openSchematicFile(target, name, filter, page);
        }
        if (selected == 1) return openSchematicPlace(target, name, filter, page);
        if (selected != 2) return openSchematicBrowser(target, filter, page);
        ll::form::ModalForm confirm(
            text("menu.schem_delete"), tr("menu.schem_delete_confirm", name),
            text("menu.confirm_yes"), text("menu.confirm_no")
        );
        confirm.sendTo(target, [name, filter, page](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
            if (result == ll::form::ModalFormSelectedButton::Upper && executeSchematic(p, Feedback(p), "delete", name)) {
                openSchematicBrowser(p, filter, page);
            } else {
                openSchematicFile(p, name, filter, page);
            }
        });
    });
}

void openSchematicBrowser(Player& player, std::string filter, size_t page) {
    std::vector<std::string> files;
    try {
        files = schematic::listFiles(filter);
    } catch (std::filesystem::filesystem_error const& error) {
        Feedback(player).error(tr("advanced.failed", error.what()));
        return openSchematicMenu(player);
    }

    constexpr size_t pageSize = 12;
    auto last = files.empty() ? size_t{0} : (files.size() - 1) / pageSize;
    page = std::min(page, last);
    ll::form::SimpleForm form(text("menu.schem_browse"),
        files.empty() ? text("menu.schem_no_results") : tr("menu.schem_page", page + 1, last + 1));
    auto begin = std::min(page * pageSize, files.size());
    auto end = std::min(begin + pageSize, files.size());
    std::vector<std::string> entries;
    for (size_t i = begin; i < end; ++i) {
        form.appendButton(files[i]);
        entries.push_back(std::move(files[i]));
    }
    int previous = -2, next = -2;
    auto row = int(entries.size());
    if (page > 0) {
        previous = row++;
        form.appendButton(text("menu.block_previous"));
    }
    if (page < last) {
        next = row++;
        form.appendButton(text("menu.block_next"));
    }
    auto search = row;
    form.appendButton(text("menu.schem_search"));
    form.appendButton(text("menu.schematics"));
    form.sendTo(player, [entries = std::move(entries), filter = std::move(filter), page, previous, next, search](
        Player& target, int selected, ll::form::FormCancelReason
    ) {
        if (selected >= 0 && size_t(selected) < entries.size()) return openSchematicFile(target, entries[size_t(selected)], filter, page);
        if (selected >= 0 && selected == previous) return openSchematicBrowser(target, filter, page - 1);
        if (selected >= 0 && selected == next) return openSchematicBrowser(target, filter, page + 1);
        if (selected == search) return openSchematicSearch(target, filter);
        openSchematicMenu(target);
    });
}

void openSchematicSave(
    Player& player, bool selection, std::string name, size_t format, bool entities = false, std::string const& error = {}
) {
    static const std::vector<std::string> formats{"cxs", "mcstructure", "schem", "litematic"};
    ll::form::CustomForm form(text(selection ? "menu.schem_export" : "menu.schem_save"));
    if (!error.empty()) form.appendLabel("§c" + error + "§r");
    form.appendInput("name", text("menu.schem_name"), "house", name);
    form.appendDropdown("format", text("menu.schem_format"), formats, format);
    if (selection) form.appendToggle("entities", text("menu.copy_entities"), entities);
    form.sendTo(player, [selection](Player& target, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return openSchematicMenu(target);
        auto name = input(*result, "name");
        auto format = std::min(number(*result, "format"), formats.size() - 1);
        std::string message;
        bool entities = selection && number(*result, "entities") != 0;
        if (!executeSchematic(
                target, Feedback(target, &message), selection ? "export" : "save", name, formats[format], false, entities
            )) {
            openSchematicSave(target, selection, std::move(name), format, entities, message);
        }
    });
}
} // namespace

void openSchematicMenu(Player& player) {
    ll::form::SimpleForm form(text("menu.schematics"));
    bool canSave = bool(getSession(player).clipboard);
    form.appendButton(text("menu.schem_browse"));
    form.appendButton(text("menu.schem_export"));
    if (canSave) form.appendButton(text("menu.schem_save"));
    form.appendButton(text("menu.clipboard"));
    form.sendTo(player, [canSave](Player& target, int selected, ll::form::FormCancelReason) {
        if (selected == 0) return openSchematicBrowser(target);
        if (selected == 1) return openSchematicSave(target, true, {}, 1);
        if (selected == 2 && canSave) return openSchematicSave(target, false, {}, 0);
        openClipboardRoot(target);
    });
}

void openAdjustMenu(Player& player) {
    ll::form::CustomForm form(text("menu.adjust_selection"));
    form.appendDropdown("mode",text("menu.action"),{text("command.shift"),text("command.outset"),text("command.inset")});
    form.appendInput("amount",text("menu.resize_amount"),"1","1");
    form.appendDropdown("direction",text("menu.resize_direction"),{text("menu.dir_me"),text("menu.dir_up"),text("menu.dir_down"),text("menu.dir_north"),text("menu.dir_south"),text("menu.dir_east"),text("menu.dir_west")});
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openSelectionRoot(target);
        auto raw=input(*result,"amount");
        int amount=0;
        auto [end,ec]=std::from_chars(raw.data(),raw.data()+raw.size(),amount);
        if(ec!=std::errc{} || end!=raw.data()+raw.size() || amount<0) {target.sendMessage(tr("selection.resize_amount"));return;}
        auto mode=number(*result,"mode");
        if(mode==1) outsetSelection(target,Feedback(target),amount,true,true);
        else if(mode==2) insetSelection(target,Feedback(target),amount);
        else {
            static constexpr std::array directions{core::Direction::me,core::Direction::up,core::Direction::down,core::Direction::north,core::Direction::south,core::Direction::east,core::Direction::west};
            auto direction=directions[std::min(number(*result,"direction"),directions.size()-1)];
            if(direction==core::Direction::me) direction = facingDirection(target);
            shiftSelection(target,Feedback(target),amount,direction);
        }
    });
}

void openCopyOptions(Player& player) {
    ll::form::CustomForm form(text("menu.copy_options"));
    form.appendToggle("cut",text("menu.cut"),false);
    form.appendToggle("entities",text("menu.copy_entities"),false);
    form.appendInput("mask",text("menu.mask_input"),"#existing","");
    form.appendInput("pattern",text("menu.cut_pattern"),"air","air");
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openClipboardRoot(target);
        auto mask=input(*result,"mask"),pattern=input(*result,"pattern");
        copySelection(target,Feedback(target),number(*result,"cut")!=0,pattern.empty() ? std::nullopt : std::optional<std::string_view>(pattern),mask.empty() ? std::nullopt : std::optional<std::string_view>(mask),number(*result,"entities")!=0);
    });
}
}
