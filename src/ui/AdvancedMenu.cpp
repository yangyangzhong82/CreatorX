#include "ui/AdvancedMenu.h"
#include "ui/Menu.h"
#include "command/AdvancedCommands.h"
#include "command/EditCommands.h"
#include "command/SelectionCommands.h"
#include "mod/Global.h"
#include "schematic/Schematic.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
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
}
void openBuildMenu(Player& player) {
    static constexpr std::array<std::string_view,12> operations{"sphere","hsphere","cyl","hcyl","pyramid","hpyramid","hollow","overlay","move","stack","count","distr"};
    ll::form::CustomForm form(text("menu.build"));
    std::vector<std::string> labels;
    for(auto name:operations) labels.push_back(text("operation."+std::string(name)));
    form.appendDropdown("operation",text("menu.action"),labels);
    form.appendInput("pattern",text("menu.pattern_input"),"stone","hand");
    form.appendInput("amount",text("menu.amount"),"5","5");
    form.appendInput("height",text("menu.height"),"10","10");
    form.appendDropdown("direction",text("menu.resize_direction"),{text("menu.dir_me"),text("menu.dir_up"),text("menu.dir_down"),text("menu.dir_north"),text("menu.dir_south"),text("menu.dir_east"),text("menu.dir_west")});
    form.appendToggle("air",text("menu.paste_skip_air"),true);
    form.appendToggle("select",text("menu.paste_select"),true);
    form.setSubmitButton(text("menu.apply"));
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openMainMenu(target);
        auto operation=operations[std::min(number(*result,"operation"),operations.size()-1)];
        auto pattern=input(*result,"pattern"),amount=input(*result,"amount"),height=input(*result,"height");
        std::string args;
        if(operation=="hollow") args=amount+" "+pattern;
        else if(operation=="overlay" || operation=="count") args=pattern;
        else if(operation=="distr") {}
        else if(operation=="move" || operation=="stack") {
            static constexpr std::array directions{"","up","down","north","south","east","west"};
            args=amount+" "+directions[std::min(number(*result,"direction"),directions.size()-1)];
            if(number(*result,"air")) args+=" -a";
            if(number(*result,"select")) args+=" -s";
        } else {
            args=pattern+" "+amount;
            if(operation=="cyl" || operation=="hcyl") args+=" "+height;
        }
        executeAdvanced(target,Feedback(target),operation,args);
    });
}
void openTransformMenu(Player& player) {
    ll::form::CustomForm form(text("menu.transform"));
    form.appendDropdown("mode",text("menu.action"),{text("menu.rotate"),text("menu.flip")});
    form.appendInput("y",text("menu.rotation_y"),"90","90");
    form.appendInput("x",text("menu.rotation_x"),"0","0");
    form.appendInput("z",text("menu.rotation_z"),"0","0");
    form.appendDropdown("axis",text("menu.resize_direction"),{text("menu.dir_east"),text("menu.dir_up"),text("menu.dir_south")});
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openMainMenu(target);
        if(number(*result,"mode")==1) {
            static constexpr std::array axes{core::Direction::east,core::Direction::up,core::Direction::south};
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
void openSchematicMenu(Player& player) {
    ll::form::CustomForm form(text("menu.schematics"));
    form.appendLabel(schematic::list());
    form.appendDropdown("mode",text("menu.action"),{text("menu.schem_save"),text("menu.schem_load"),text("menu.schem_delete")});
    form.appendInput("name",text("menu.schem_name"),"house","house");
    form.appendDropdown("format",text("menu.schem_format"),{"cxs","mcstructure"});
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openMainMenu(target);
        static constexpr std::array modes{"save","load","delete"};
        auto action=std::string(modes[std::min(number(*result,"mode"),modes.size()-1)]);
        auto name=input(*result,"name");
        auto format=number(*result,"format") ? "mcstructure" : "cxs";
        if(action=="delete") {
            ll::form::ModalForm confirm(text("menu.schem_delete"),tr("menu.schem_delete_confirm",name),text("menu.confirm_yes"),text("menu.confirm_no"));
            confirm.sendTo(target,[name,format](Player& p,ll::form::ModalFormResult selected,ll::form::FormCancelReason) {
                if(selected==ll::form::ModalFormSelectedButton::Upper) executeAdvanced(p,Feedback(p),"schem","delete "+name+" "+format);
            });
        } else executeAdvanced(target,Feedback(target),"schem",action+" "+name+" "+format);
    });
}

void openAdjustMenu(Player& player) {
    ll::form::CustomForm form(text("menu.adjust_selection"));
    form.appendDropdown("mode",text("menu.action"),{text("command.shift"),text("command.outset"),text("command.inset")});
    form.appendInput("amount",text("menu.resize_amount"),"1","1");
    form.appendDropdown("direction",text("menu.resize_direction"),{text("menu.dir_me"),text("menu.dir_up"),text("menu.dir_down"),text("menu.dir_north"),text("menu.dir_south"),text("menu.dir_east"),text("menu.dir_west")});
    form.sendTo(player,[](Player& target,ll::form::CustomFormResult const& result,ll::form::FormCancelReason) {
        if(!result) return openMainMenu(target);
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
            if(direction==core::Direction::me) {
                auto v=target.getViewVector();
                if(std::abs(v.y)>=std::max(std::abs(v.x),std::abs(v.z))) direction=v.y>0 ? core::Direction::up : core::Direction::down;
                else if(std::abs(v.x)>=std::abs(v.z)) direction=v.x>0 ? core::Direction::east : core::Direction::west;
                else direction=v.z>0 ? core::Direction::south : core::Direction::north;
            }
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
        if(!result) return openMainMenu(target);
        auto mask=input(*result,"mask"),pattern=input(*result,"pattern");
        copySelection(target,Feedback(target),number(*result,"cut")!=0,pattern.empty() ? std::nullopt : std::optional<std::string_view>(pattern),mask.empty() ? std::nullopt : std::optional<std::string_view>(mask),number(*result,"entities")!=0);
    });
}
}
