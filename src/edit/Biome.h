#pragma once

#include "core/math/Vec3i.h"
#include <string>
#include <vector>

class Biome;
class BlockSource;
class Level;
class ChunkPos;

namespace creatorx::edit {
class ChangeSet;

Biome const* findBiome(Level& level, std::string name);
std::vector<std::string> registeredBiomeIds(Level& level);
bool writeBiome(BlockSource& region, core::Vec3i pos, Biome const& biome, ChangeSet* changes = nullptr);
void refreshBiomes(Level& level, int dimension, std::vector<ChunkPos> const& chunks);
} // namespace creatorx::edit
