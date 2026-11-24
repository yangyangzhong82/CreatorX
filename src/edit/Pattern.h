#pragma once

#include "core/region/Box.h"

#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>

class Block;
class Player;
class BlockSource;

namespace creatorx::edit {
struct Clipboard;

Block const& airBlock();

// 配置 limits.disallowedBlocks 中禁止通过 CreatorX 放置的方块。
bool isDisallowed(Block const& block);
void refreshBlockBlacklist();

std::vector<std::string> registeredBlockIds();

// 解析单个方块：hand / offhand / 名称[状态]。失败时 error 为玩家可读的说明。
Block const* parseBlock(std::string_view text, Player const& player, std::string& error);

// 图案：`stone`、`wool[color=red]`、`60%stone,30%dirt,andesite`、`hand`。
class Pattern {
public:
    static std::optional<Pattern> parse(std::string_view text, Player const& player, std::string& error);
    static Pattern                single(Block const& block);

    Block const& apply(std::mt19937& random) const;
    Block const& apply(std::mt19937& random, core::Vec3i position, core::Box const& bounds, Block const& current, bool* preserveData = nullptr, bool* skip = nullptr) const;
    bool         isAllowed() const;

private:
    std::vector<Block const*> mBlocks;
    std::vector<double>       mCumulative;
    std::vector<Pattern>      mChildren;
    std::vector<Pattern>      mChoices;
    int                       mAxis = 1;
    std::shared_ptr<Clipboard const> mClipboard;
    std::function<Block const&(Block const&)> mModify;
};

// 掩码：`stone,dirt`、`!air`、`#existing`、`#solid`、`#air`，空格分隔表示交集。
class Mask {
public:
    static std::optional<Mask> parse(std::string_view text, Player const& player, std::string& error);

    bool test(Block const& block) const;
    bool test(Block const& block, BlockSource& region, core::Vec3i position) const;
    bool needsNeighbors() const;

private:
    enum class Kind { blocks, existing, solid, air, surface, chance, biome, tag, above, below };
    struct Matcher {
        std::string  typeName;
        Block const* exact = nullptr; // 写了状态时按完整方块比较
    };
    struct Term {
        Kind                 kind   = Kind::blocks;
        bool                 negate = false;
        std::vector<Matcher> matchers;
        std::string          value;
        double               chance = 100;
        std::shared_ptr<Mask> adjacent;
    };
    std::vector<Term> mTerms;
    bool testAt(Block const& block, BlockSource* region, core::Vec3i position) const;
};

// 按空白切分，方括号和引号内的空白保留。
std::vector<std::string_view> splitTokens(std::string_view text);

} // namespace creatorx::edit
