#pragma once
class Player;
namespace creatorx {
void registerEnhancementProtocol();
void enableEnhancements();
void refreshEnhancementOptions();
void disableEnhancements();
void unregisterEnhancementProtocol();
bool hasEnhancedClient(Player const& player);
}
