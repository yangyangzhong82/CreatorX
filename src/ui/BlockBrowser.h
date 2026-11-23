#pragma once

#include <functional>
#include <string>

class Player;

namespace creatorx::ui {

using BlockPicked = std::function<void(Player&, std::string)>;
using FillSubmitted = std::function<void(Player&, size_t, std::string)>;

// 点选后带回方块 ID，返回或关闭时带回空串。
void openBlockBrowser(Player& player, BlockPicked picked);
void openFillDdui(Player& player, size_t shape, std::string pattern, std::string error, FillSubmitted submitted);
void closeBlockBrowsers();

} // namespace creatorx::ui
