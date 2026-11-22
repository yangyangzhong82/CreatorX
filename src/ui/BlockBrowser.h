#pragma once

#include <functional>
#include <string>

class Player;

namespace creatorx::ui {

using BlockPicked = std::function<void(Player&, std::string)>;

// 点选后带回方块 ID，返回或关闭时带回空串。
void openBlockBrowser(Player& player, BlockPicked picked);
void closeBlockBrowsers();

} // namespace creatorx::ui
