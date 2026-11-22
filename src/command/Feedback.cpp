#include "command/Feedback.h"
#include "net/Enhancements.h"

#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"

namespace creatorx {

Feedback::Feedback(Player& player, std::string* errors) : mPlayer(&player), mErrors(errors) {
    noteHudActivity(player);
}

void Feedback::success(std::string_view text) const {
    if (mOutput) mOutput->success(text);
    else if (mPlayer) mPlayer->sendMessage(text);
}

void Feedback::error(std::string_view text) const {
    if (mOutput) mOutput->error(text);
    else if (mErrors) *mErrors = text;
    else if (mPlayer) mPlayer->sendMessage("§c" + std::string(text));
}

} // namespace creatorx
