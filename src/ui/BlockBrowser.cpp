#include "ui/BlockBrowser.h"

#include "edit/Pattern.h"
#include "mod/Global.h"
#include "session/PlayerSession.h"

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "ll/api/ui/form/CustomForm.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/locale/I18n.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace creatorx::ui {
namespace {

std::string text(std::string_view key) { return std::string(tr(key)); }

std::string shortName(std::string const& id) { return id.starts_with("minecraft:") ? id.substr(10) : id; }

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return value;
}

struct CommonBlock {
    char const* id;
    char const* texture;
};

constexpr std::array<CommonBlock, 26> commonBlocks{
    {CommonBlock{"stone", "stone"},
     {"cobblestone", "cobblestone"},
     {"stone_bricks", "stonebrick"},
     {"smooth_stone", "stone_slab_top"},
     {"andesite", "stone_andesite"},
     {"diorite", "stone_diorite"},
     {"granite", "stone_granite"},
     {"deepslate", "deepslate/deepslate"},
     {"dirt", "dirt"},
     {"grass_block", "grass_side_carried"},
     {"sand", "sand"},
     {"gravel", "gravel"},
     {"oak_planks", "planks_oak"},
     {"spruce_planks", "planks_spruce"},
     {"birch_planks", "planks_birch"},
     {"oak_log", "log_oak"},
     {"glass", "glass"},
     {"white_concrete", "concrete_white"},
     {"white_wool", "wool_colored_white"},
     {"quartz_block", "quartz_block_side"},
     {"bricks", "brick"},
     {"sea_lantern", "sea_lantern"},
     {"glowstone", "glowstone"},
     {"obsidian", "obsidian"},
     {"water", "water_still"},
     {"air", ""}}
};

struct Category {
    char const*                   key;
    char const*                   icon;
    std::vector<std::string_view> keywords;
};

std::array<Category, 7> const categories{
    {{"menu.blocks_stone",
      "textures/blocks/stone",
      {"stone",         "granite",   "diorite", "andesite",   "deepslate",  "tuff",       "calcite",
       "brick",         "quartz",    "ore",     "copper",     "iron_block", "gold_block", "diamond_block",
       "emerald_block", "netherite", "basalt",  "blackstone", "obsidian",   "prismarine"}},
     {"menu.blocks_wood",
      "textures/blocks/planks_oak",
      {"planks",
       "log",
       "wood",
       "stem",
       "hyphae",
       "bamboo",
       "oak_",
       "spruce_",
       "birch_",
       "jungle_",
       "acacia_",
       "mangrove_",
       "cherry_",
       "crimson_",
       "warped_"}},
     {"menu.blocks_nature",
      "textures/blocks/grass_side_carried",
      {"dirt",     "grass",  "sand",   "gravel",  "clay", "mud",        "snow",     "ice",
       "leaves",   "flower", "tulip",  "sapling", "moss", "vine",       "fern",     "bush",
       "mushroom", "coral",  "cactus", "water",   "lava", "netherrack", "end_stone"}},
     {"menu.blocks_color",
      "textures/blocks/wool_colored_red",
      {"wool", "concrete", "terracotta", "carpet", "hardened_clay"}},
     {"menu.blocks_glass_light",
      "textures/blocks/glass",
      {"glass", "lantern", "glowstone", "torch", "light", "froglight", "end_rod", "beacon"}},
     {"menu.blocks_decoration",
      "textures/blocks/bookshelf",
      {"stairs",
       "slab",
       "wall",
       "fence",
       "door",
       "trapdoor",
       "bed",
       "bookshelf",
       "pot",
       "candle",
       "chain",
       "bar",
       "sign",
       "banner",
       "head",
       "skull"}},
     {"menu.blocks_functional",
      "textures/blocks/crafting_table_front",
      {"chest",    "barrel",     "furnace", "table",   "redstone",      "piston",         "observer",
       "hopper",   "dispenser",  "dropper", "rail",    "button",        "lever",          "pressure_plate",
       "repeater", "comparator", "target",  "crafter", "command_block", "structure_block"}}}
};

bool matchesCategory(std::string const& id, Category const& category) {
    return std::ranges::any_of(category.keywords, [&id](auto word) { return id.find(word) != std::string::npos; });
}

struct Entry {
    std::string id;
    std::string name;
    std::string icon;
};

using Entries = std::shared_ptr<std::vector<Entry> const>;

struct Browser {
    BlockPicked        picked;
    std::vector<Entry> all;
    std::vector<Entry> common;
    std::vector<Entry> recent;
};

using State  = std::shared_ptr<Browser const>;
using Action = std::function<void(Player&)>;

void sendMenu(ll::form::SimpleForm& form, Player& player, State state, std::vector<Action> actions) {
    form.sendTo(player, [state, actions = std::move(actions)](Player& p, int selected, ll::form::FormCancelReason) {
        if (selected < 0) return state->picked(p, {});
        actions[size_t(selected)](p);
    });
}

void addButton(
    ll::form::SimpleForm& form,
    std::vector<Action>&  actions,
    std::string           label,
    std::string           icon,
    Action                action
) {
    if (icon.empty()) form.appendButton(label);
    else form.appendButton(label, icon, "path");
    actions.push_back(std::move(action));
}

void openHome(Player& player, State state);
void openSearch(Player& player, State state, std::string query = {});

void openResults(
    Player&     player,
    State       state,
    Entries     entries,
    std::string title,
    size_t      page  = 0,
    std::string query = {}
) {
    constexpr size_t     pageSize  = 16;
    auto                 pageCount = std::max<size_t>(1, (entries->size() + pageSize - 1) / pageSize);
    ll::form::SimpleForm form(
        title,
        entries->empty() ? text("menu.blocks_empty") : tr("menu.blocks_page", entries->size(), page + 1, pageCount)
    );
    std::vector<Action> actions;
    addButton(form, actions, text("menu.blocks_home"), "textures/items/book_writable", [state](Player& p) {
        openHome(p, state);
    });
    addButton(
        form,
        actions,
        text("menu.block_search_submit"),
        "textures/items/compass_item",
        [state, query](Player& p) { openSearch(p, state, query); }
    );
    auto end = std::min((page + 1) * pageSize, entries->size());
    for (size_t i = page * pageSize; i < end; ++i) {
        auto const& entry = (*entries)[i];
        auto        label = entry.name == shortName(entry.id) ? entry.name : entry.name + "\n§8" + shortName(entry.id);
        addButton(form, actions, std::move(label), entry.icon, [state, entries, i](Player& p) {
            state->picked(p, (*entries)[i].id);
        });
    }
    if (page > 0) {
        addButton(
            form,
            actions,
            text("menu.block_previous"),
            "textures/items/arrow",
            [state, entries, title, page, query](Player& p) { openResults(p, state, entries, title, page - 1, query); }
        );
    }
    if (page + 1 < pageCount) {
        addButton(
            form,
            actions,
            text("menu.block_next"),
            "textures/items/arrow",
            [state, entries, title, page, query](Player& p) { openResults(p, state, entries, title, page + 1, query); }
        );
    }
    addButton(form, actions, text("menu.block_search_back"), "textures/items/arrow", [state](Player& p) {
        state->picked(p, {});
    });
    sendMenu(form, player, state, std::move(actions));
}

void openSearch(Player& player, State state, std::string query) {
    ll::form::CustomForm form(text("menu.block_search_title"));
    form.appendInput("query", text("menu.block_search_query"), text("menu.block_search_example"), query);
    form.setSubmitButton(text("menu.block_search_submit"));
    form.sendTo(player, [state](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return openHome(p, state);
        auto               query  = lower(std::get<std::string>(result->at("query")));
        auto               tokens = edit::splitTokens(query);
        std::vector<Entry> matches;
        for (auto const& entry : state->all) {
            auto searchable = lower(entry.name + " " + entry.id);
            if (std::ranges::all_of(tokens, [&searchable](auto token) {
                    return searchable.find(token) != std::string::npos;
                })) {
                matches.push_back(entry);
            }
        }
        auto rank = [&query](Entry const& entry) {
            auto id   = shortName(entry.id);
            auto name = lower(entry.name);
            if (id == query || entry.id == query || name == query) return 0;
            return id.starts_with(query) || name.starts_with(query) ? 1 : 2;
        };
        std::stable_sort(matches.begin(), matches.end(), [&rank](auto const& a, auto const& b) {
            return rank(a) < rank(b);
        });
        openResults(
            p,
            state,
            std::make_shared<std::vector<Entry>>(std::move(matches)),
            text("menu.block_search_title"),
            0,
            query
        );
    });
}

void openHome(Player& player, State state) {
    ll::form::SimpleForm form(text("menu.blocks_title"), text("menu.blocks_hint"));
    std::vector<Action>  actions;
    auto                 addList = [&](std::string_view key, char const* icon, std::vector<Entry> entries) {
        auto title = text(key);
        auto label = tr("menu.blocks_category", title, entries.size());
        addButton(
            form,
            actions,
            std::move(label),
            icon,
            [state, title, entries = std::make_shared<std::vector<Entry>>(std::move(entries))](Player& p) {
                openResults(p, state, entries, title);
            }
        );
    };
    addList("menu.blocks_common", "textures/blocks/brick", state->common);
    addList("menu.blocks_recent", "textures/items/clock_item", state->recent);
    addButton(form, actions, text("menu.block_search_submit"), "textures/items/compass_item", [state](Player& p) {
        openSearch(p, state);
    });
    for (auto const& category : categories) {
        std::vector<Entry> entries;
        for (auto const& entry : state->all)
            if (matchesCategory(entry.id, category)) entries.push_back(entry);
        addList(category.key, category.icon, std::move(entries));
    }
    addList("menu.blocks_all", "textures/blocks/stonebrick", state->all);
    addButton(form, actions, text("menu.block_search_back"), "textures/items/arrow", [state](Player& p) {
        state->picked(p, {});
    });
    sendMenu(form, player, state, std::move(actions));
}


// 每个方块独占一行；只更新内容，不关闭重开 DDUI。
struct BlockRow {
    ll::ui::ObservableString  label{std::string{}};
    ll::ui::ObservableString  tooltip{std::string{}};
    ll::ui::ObservableBoolean visible{false};
};

struct LiveBrowser {
    static constexpr size_t pageSize = 16;

    mce::UUID                      uuid;
    State                          catalog;
    ll::ui::CustomForm             form;
    ll::ui::ObservableNumber       category{0.0, {.clientWritable = true}};
    ll::ui::ObservableString       query{std::string{}, {.clientWritable = true}};
    ll::ui::ObservableString       summary{std::string{}};
    ll::ui::ObservableBoolean      previousDisabled{true};
    ll::ui::ObservableBoolean      nextDisabled{true};
    std::array<BlockRow, pageSize> rows;
    std::vector<Entry const*>      matches;
    size_t                         page{};
    std::string                    selected;
    bool                           standardList{};
    bool                           finished{};

    LiveBrowser(Player& player, State state)
    : uuid(player.getUuid()),
      catalog(std::move(state)),
      form(player, text("menu.blocks_title")) {}

    void updatePage() {
        auto pages = std::max<size_t>(1, (matches.size() + pageSize - 1) / pageSize);
        page       = std::min(page, pages - 1);
        summary.setData(
            matches.empty() ? text("menu.blocks_empty") : tr("menu.blocks_page", matches.size(), page + 1, pages)
        );
        previousDisabled.setData(page == 0);
        nextDisabled.setData(page + 1 == pages);
        for (size_t i = 0; i < rows.size(); ++i) {
            auto  index = page * pageSize + i;
            auto& row   = rows[i];
            if (index >= matches.size()) {
                row.visible.setData(false);
                continue;
            }
            auto const& entry = *matches[index];
            row.label.setData(entry.name == shortName(entry.id) ? entry.name : entry.name + "\n" + shortName(entry.id));
            row.tooltip.setData(entry.id);
            row.visible.setData(true);
        }
    }

    void filter() {
        auto value = category.getData();
        if (!std::isfinite(value) || value < 0 || value > double(categories.size() + 2) || std::floor(value) != value)
            return;
        auto        chosen = size_t(value);
        auto const& source = chosen == 0 ? catalog->common : chosen == 1 ? catalog->recent : catalog->all;
        auto        search = lower(query.getData());
        auto        tokens = edit::splitTokens(search);
        matches.clear();
        for (auto const& entry : source) {
            if (chosen >= 2 && chosen < categories.size() + 2 && !matchesCategory(entry.id, categories[chosen - 2]))
                continue;
            auto searchable = lower(entry.name + " " + entry.id);
            if (std::ranges::all_of(tokens, [&searchable](auto token) {
                    return searchable.find(token) != std::string::npos;
                }))
                matches.push_back(&entry);
        }
        if (!tokens.empty()) {
            auto rank = [&search](Entry const* entry) {
                auto name = lower(entry->name);
                auto id   = shortName(entry->id);
                if (id == search || entry->id == search || name == search) return 0;
                return id.starts_with(search) || name.starts_with(search) ? 1 : 2;
            };
            std::stable_sort(matches.begin(), matches.end(), [&rank](auto a, auto b) { return rank(a) < rank(b); });
        }
        page = 0;
        updatePage();
    }

    void close() {
        if (auto closed = form.close(); !closed) {
            selected.clear();
            standardList = false;
            summary.setData(tr("menu.blocks_dd_ui_error", closed.error().message()));
        }
    }
};

std::unordered_map<mce::UUID, std::shared_ptr<LiveBrowser>> liveBrowsers;

Player* findPlayer(mce::UUID const& uuid) {
    auto level = ll::service::getLevel();
    return level ? level->getPlayer(uuid) : nullptr;
}

void finishBrowser(std::shared_ptr<LiveBrowser> const& state, bool fallback = false) {
    if (state->finished) return;
    state->finished = true;
    liveBrowsers.erase(state->uuid);
    ll::thread::ServerThreadExecutor::getDefault().execute([uuid     = state->uuid,
                                                            catalog  = state->catalog,
                                                            selected = state->selected,
                                                            standard = state->standardList || fallback] {
        if (auto* player = findPlayer(uuid)) {
            if (standard) openHome(*player, catalog);
            else catalog->picked(*player, selected);
        }
    });
}

void openLiveBrowser(Player& player, State catalog) {
    auto                                  state = std::make_shared<LiveBrowser>(player, std::move(catalog));
    auto                                  weak  = std::weak_ptr{state};
    std::vector<ll::ui::DropdownItemData> items{
        {text("menu.blocks_common"), 0.0, {}},
        {text("menu.blocks_recent"), 1.0, {}}
    };
    for (size_t i = 0; i < categories.size(); ++i) items.push_back({text(categories[i].key), double(i + 2), {}});
    items.push_back({text("menu.blocks_all"), double(categories.size() + 2), {}});
    state->form.closeButton()
        .dropdown(text("menu.blocks_filter"), state->category, std::move(items))
        .textField(text("menu.blocks_live_search"), state->query, {.description = text("menu.blocks_live_hint")})
        .label(state->summary)
        .button(
            text("menu.block_previous"),
            [weak] {
                if (auto state = weak.lock(); state && !state->finished && state->page > 0) {
                    --state->page;
                    state->updatePage();
                }
            },
            {.disabled = state->previousDisabled}
        )
        .button(
            text("menu.block_next"),
            [weak] {
                if (auto state = weak.lock();
                    state && !state->finished && (state->page + 1) * LiveBrowser::pageSize < state->matches.size()) {
                    ++state->page;
                    state->updatePage();
                }
            },
            {.disabled = state->nextDisabled}
        )
        .divider();
    for (size_t i = 0; i < state->rows.size(); ++i) {
        auto& row = state->rows[i];
        state->form.button(
            row.label,
            [weak, i] {
                auto state = weak.lock();
                if (!state || state->finished || !state->selected.empty()) return;
                auto index = state->page * LiveBrowser::pageSize + i;
                if (index >= state->matches.size()) return;
                state->selected = state->matches[index]->id;
                state->close();
            },
            {.tooltip = row.tooltip, .visible = row.visible}
        );
    }
    state->form.divider()
        .button(
            text("menu.blocks_standard_list"),
            [weak] {
                if (auto state = weak.lock(); state && !state->finished) {
                    state->standardList = true;
                    state->close();
                }
            }
        )
        .button(text("menu.block_search_back"), [weak] {
            if (auto state = weak.lock(); state && !state->finished) state->close();
        });
    state->query.subscribe([weak](auto const&) {
        if (auto state = weak.lock(); state && !state->finished) state->filter();
    });
    state->category.subscribe([weak](auto const&) {
        if (auto state = weak.lock(); state && !state->finished) state->filter();
    });
    state->filter();
    liveBrowsers[state->uuid] = state;
    auto shown                = state->form.show([weak](ll::ui::CustomForm::Result result) {
        auto state = weak.lock();
        if (!state || state->finished) return;
        bool fallback = !result || *result == ll::ui::ScreenCloseReason::InvalidForm;
        if (result && *result == ll::ui::ScreenCloseReason::UserBusy) {
            state->finished = true;
            liveBrowsers.erase(state->uuid);
            if (auto* player = findPlayer(state->uuid)) player->sendMessage(text("menu.blocks_busy"));
            return;
        }
        if (fallback) {
            if (auto* player = findPlayer(state->uuid)) player->sendMessage(text("menu.blocks_dd_ui_fallback"));
        }
        finishBrowser(state, fallback);
    });
    if (!shown) {
        player.sendMessage(text("menu.blocks_dd_ui_fallback"));
        finishBrowser(state, true);
    }
}


} // namespace

void openBlockBrowser(Player& player, BlockPicked picked) {
    auto browser    = std::make_shared<Browser>();
    browser->picked = std::move(picked);
    auto& i18n      = ::getI18n();
    auto  locale    = i18n.getLocaleFor(config.language);
    for (auto const& id : edit::registeredBlockIds()) {
        auto block = Block::tryGetFromRegistry(HashedString(id));
        if (!block || edit::isDisallowed(*block)) continue;
        auto name = shortName(id);
        if (locale) {
            auto key       = block->getDescriptionId();
            auto localized = i18n.get(key, locale);
            if (!localized.empty() && localized != key) name = std::move(localized);
        }
        std::string icon;
        auto        common = std::ranges::find_if(commonBlocks, [&id](auto const& entry) {
            return id == "minecraft:" + std::string(entry.id);
        });
        if (common != commonBlocks.end()) {
            name = text("menu.block_name." + std::string(common->id));
            if (*common->texture) icon = "textures/blocks/" + std::string(common->texture);
        } else {
            auto category =
                std::ranges::find_if(categories, [&id](auto const& entry) { return matchesCategory(id, entry); });
            if (category != categories.end()) icon = category->icon;
        }
        browser->all.push_back({id, std::move(name), std::move(icon)});
    }
    // 常用项按建材顺序排列；注册表里不存在的方块不展示。
    for (auto const& common : commonBlocks) {
        auto it = std::ranges::find(browser->all, "minecraft:" + std::string(common.id), &Entry::id);
        if (it != browser->all.end()) browser->common.push_back(*it);
    }
    for (auto const& pattern : getSession(player).recentPatterns) {
        auto id = pattern.find(':') == std::string::npos ? "minecraft:" + pattern : pattern;
        auto it = std::ranges::find(browser->all, id, &Entry::id);
        if (it != browser->all.end()) browser->recent.push_back(*it);
    }
    openLiveBrowser(player, std::move(browser));
}


void closeBlockBrowsers() {
    auto active = std::move(liveBrowsers);
    liveBrowsers.clear();
    for (auto const& [_, state] : active) {
        state->finished = true;
        (void)state->form.close();
    }
}


} // namespace creatorx::ui
