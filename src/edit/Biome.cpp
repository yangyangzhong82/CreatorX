#include "edit/Biome.h"
#include "edit/ChangeSet.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkBlockPos.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/biome/registry/BiomeRegistry.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/LevelChunkDataField.h"

#include <algorithm>
#include <cctype>

namespace creatorx::edit {

Biome const* findBiome(Level& level, std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    auto const& biomes = *level.getBiomeRegistry().mBiomesByName;
    if (auto it = biomes.find(HashedString::computeHash(name)); it != biomes.end()) return it->second.get();
    // 部分原版注册名称不包含默认命名空间。
    if (name.starts_with("minecraft:")) {
        auto it = biomes.find(HashedString::computeHash(std::string_view(name).substr(10)));
        if (it != biomes.end()) return it->second.get();
    }
    return nullptr;
}

std::vector<std::string> registeredBiomeIds(Level& level) {
    std::vector<std::string> result;
    for (auto const& [hash, biome] : *level.getBiomeRegistry().mBiomesByName) {
        auto id = biome->mHash->getString();
        result.push_back(id);
        if (id.starts_with("minecraft:")) result.push_back(id.substr(10));
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool writeBiome(BlockSource& region, core::Vec3i p, Biome const& biome, ChangeSet* changes) {
    if (p.y < region.getMinHeight() || p.y >= region.getMaxHeight()) return false;
    BlockPos pos{p.x, p.y, p.z};
    auto* chunk = region.getChunk(ChunkPos{pos});
    if (!chunk) return false;
    ChunkBlockPos local{pos, region.getMinHeight()};
    auto const& old = chunk->getBiome(local);
    if (&old == &biome) return false;
    chunk->_setBiome(biome, local, false);
    // 群系存储也需要进入区块的脏数据保存流程。
    chunk->mSaveNonActorDataIfDirty = true;
    auto& dirty = *chunk->mFullChunkDirtyTicksCounters[int(LevelChunkDataField::BiomeState)];
    dirty.totalTime = std::max(1, int(dirty.totalTime));
    dirty.lastChange = 0;
    if (changes) changes->recordBiome(p, old.mHash->getString(), biome.mHash->getString());
    return true;
}

void refreshBiomes(Level& level, int dimension, std::vector<ChunkPos> const& chunks) {
    if (chunks.empty()) return;
    level.forEachPlayer(std::function<bool(Player&)>([&](Player& player) {
        if (int(player.getDimensionId()) == dimension) player.addChunksToQueue(chunks, true);
        return true;
    }));
}

} // namespace creatorx::edit
