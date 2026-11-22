#pragma once
#include "edit/Clipboard.h"
#include "edit/Pattern.h"
class Player;
namespace creatorx {
struct PastePlacement {
    std::shared_ptr<edit::Clipboard const> clipboard;
    core::Vec3i min;
    bool skipAir;
    bool select;
    bool entities;
    std::optional<edit::Mask> mask;
};

void showPastePreview(Player& player, PastePlacement placement);
PastePlacement const* getPastePreview(Player& player);
bool cancelPastePreview(Player& player);
void enablePreviews();
void disablePreviews();
}
