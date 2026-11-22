#pragma once
class Player;
namespace creatorx {
void noteHudActivity(Player const& player);
void setHudPreferences(Player const& player, bool show, int idleSeconds);
#ifdef LL_PLAT_C
void noteLocalHudActivity();
#endif

void registerEnhancementProtocol();
void enableEnhancements();
void refreshEnhancementOptions();
void disableEnhancements();
void unregisterEnhancementProtocol();
bool hasEnhancedClient(Player const& player);
}
