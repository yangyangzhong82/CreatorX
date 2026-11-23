#pragma once

#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

class Block;
class Player;

namespace creatorx::edit {

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
    bool         isAllowed() const;

private:
    std::vector<Block const*> mBlocks;
    std::vector<double>       mCumulative;
};

// 掩码：`stone,dirt`、`!air`、`#existing`、`#solid`、`#air`，空格分隔表示交集。
class Mask {
public:
    static std::optional<Mask> parse(std::string_view text, Player const& player, std::string& error);

    bool test(Block const& block) const;

private:
    enum class Kind { blocks, existing, solid, air };
    struct Matcher {
        std::string  typeName;
        Block const* exact = nullptr; // 写了状态时按完整方块比较
    };
    struct Term {
        Kind                 kind   = Kind::blocks;
        bool                 negate = false;
        std::vector<Matcher> matchers;
    };
    std::vector<Term> mTerms;
};

// 按空白切分，方括号和引号内的空白保留。
std::vector<std::string_view> splitTokens(std::string_view text);

} // namespace creatorx::edit
