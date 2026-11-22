#pragma once

class Player;

namespace creatorx::ui {

// CreatorX 主菜单（/cx menu 或蹲下右键魔杖）。所有常用操作都能在表单里完成，适合手机端。
void openMainMenu(Player& player);
void openClipboardRoot(Player& player);
void openSelectionRoot(Player& player);

} // namespace creatorx::ui
