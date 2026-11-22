#pragma once
#include "command/Feedback.h"
#include <string_view>
class Player;
namespace creatorx {
void executeAdvanced(Player& player,Feedback const& output,std::string_view operation,std::string_view arguments);
void enableAdvancedCommands();
void disableAdvancedCommands();
}
