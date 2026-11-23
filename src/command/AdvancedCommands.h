#pragma once
#include "command/Feedback.h"
#include <string_view>
class Player;
namespace creatorx {
void executeAdvanced(Player& player,Feedback const& output,std::string_view operation,std::string_view arguments);
bool executeSchematic(
    Player& player, Feedback const& output, std::string_view action, std::string name,
    std::string format = "cxs", bool skipAir = false, bool entities = false
);
void enableAdvancedCommands();
void disableAdvancedCommands();
}
