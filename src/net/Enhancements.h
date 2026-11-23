#pragma once
#include <string_view>

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
bool syncBrushBinding(Player const& player, std::string_view item, bool handlesLeft);
}
