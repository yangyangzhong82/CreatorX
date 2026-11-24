#include "edit/Jobs.h"
#include "edit/Biome.h"

#include "mod/Global.h"
#include "session/PlayerSession.h"
#include "schematic/Schematic.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/level/levelgen/structure/StructureTemplate.h"
#include "mc/world/level/levelgen/structure/BoundingBox.h"
#include "mc/world/phys/AABB.h"
#include "mc/legacy/ActorUniqueID.h"
#include <set>
#include "storage/Nbt.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/chunk/ChunkViewSource.h"

#include <chrono>
#include <cmath>
#include <random>
#include <map>
#include <utility>

namespace creatorx::edit {
namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

void tell(Player* player, std::string const& message) {
    if (player) player->sendMessage(message);
}

// 每个任务只持有当前需要临时加载的区块视图，离开区块或结束任务时释放。
class ChunkAccess {
public:
    enum class State { ready, loading, failed };

    void beginTick() { mReady.reset(); }
    void setNeighbors(bool enabled) { mNeighbors=enabled; }

    State prepare(Level& level, BlockSource& region, mce::UUID const& owner, core::Vec3i p) {
        auto chunk = ChunkPos(BlockPos{p.x, p.y, p.z});
        if (mReady == chunk) return State::ready;
        if (mRequested != chunk) {
            mView.reset();
            mDeferredChunks.clear();
            mRequested.reset();
        }

        auto& source = region.getChunkSource();
        if (!source.isWithinWorldLimit(chunk)) return State::failed;
        std::vector<ChunkPos> missing;
        int radius=mNeighbors ? 1 : 0;
        for(int z=-radius;z<=radius;++z) for(int x=-radius;x<=radius;++x) {
            auto neighbor=chunk;
            neighbor.x+=x;neighbor.z+=z;
            if(source.isWithinWorldLimit(neighbor) && !source.getAvailableChunk(neighbor)) missing.push_back(neighbor);
        }
        if (missing.empty()) {
            mReady = chunk;
            return State::ready;
        }
        if (mRequested) return Clock::now() < mDeadline ? State::loading : State::failed;

        if (auto* player = level.getPlayer(owner)) mView = player->$_createChunkSource(source);
        if (mView) {
            auto x = chunk.x * 16;
            auto z = chunk.z * 16;
            mView->move(
                BlockPos{x-radius*16, p.y, z-radius*16},
                BlockPos{x+15+radius*16, p.y, z+15+radius*16},
                false,
                ChunkSourceViewGenerateMode::GenerateAll,
                [](auto) {},
                nullptr
            );
        } else {
            for(auto const& neighbor:missing) mDeferredChunks.push_back(source.getOrLoadChunk(neighbor, ChunkSource::LoadMode::Deferred, false));
        }
        mRequested = chunk;
        mDeadline  = Clock::now() + std::chrono::seconds(30);
        return State::loading;
    }

private:
    std::optional<ChunkPos>          mReady;
    std::optional<ChunkPos>          mRequested;
    std::shared_ptr<ChunkViewSource> mView;
    std::vector<std::shared_ptr<LevelChunk>> mDeferredChunks;
    bool mNeighbors=false;
    Clock::time_point                mDeadline{};
};

std::string chunkError(core::Vec3i p) { return tr("job.chunk_load_failed", p.x >> 4, p.z >> 4); }

// 把本次改动记入玩家历史并汇报结果。
void finishEdit(
    mce::UUID const&   owner,
    Player*            player,
    exec::JobEnd       end,
    ChangeSet&         changes,
    std::uint64_t      changed,
    std::string const& error,
    Clock::time_point  start
) {
    if (auto* session = findSession(owner)) session->history.push(std::move(changes));
    saveSession(owner);
    switch (end) {
    case exec::JobEnd::completed:
        tell(player, tr("edit.done", changed, fmt::format("{:.1f}", secondsSince(start))));
        break;
    case exec::JobEnd::cancelled:
        tell(player, tr("edit.cancelled", changed));
        break;
    case exec::JobEnd::failed:
        tell(player, error);
        if (changed) tell(player, tr("edit.partial", changed));
        break;
    }
}

// 在任务创建时捕获全局掩码，撤销和复制读取不受它影响。
std::optional<Mask> globalMask(mce::UUID const& owner) {
    auto session=findSession(owner);
    return session ? session->globalMask : std::nullopt;
}

bool maskNeighbors(std::optional<Mask> const& mask) { return mask && mask->needsNeighbors(); }

template<class... Args>
bool writeMasked(std::optional<Mask> const& mask, BlockSource& region, core::Vec3i p, Args&&... args) {
    return (!mask || mask->test(region.getBlock({p.x,p.y,p.z}),region,p))
        && writeBlock(region,p,std::forward<Args>(args)...);
}

bool writePattern(std::optional<Mask> const& global, BlockSource& region, core::Vec3i p,
                  Pattern const& pattern, core::Box const& bounds, std::mt19937& random, ChangeSet& changes) {
    auto const& old=region.getBlock({p.x,p.y,p.z});
    bool preserveData=false,skip=false;
    auto const& next=pattern.apply(random,p,bounds,old,&preserveData,&skip);
    if(skip) return false;
    if(!preserveData) return writeMasked(global,region,p,next,airBlock(),nullptr,&changes,&old);
    CompoundTag entity;
    CompoundTag const* saved=nullptr;
    if(auto actor=region.getBlockEntity({p.x,p.y,p.z})) if(actor->save(entity,SaveContext{})) saved=&entity;
    return writeMasked(global,region,p,next,region.getExtraBlock({p.x,p.y,p.z}),saved,&changes,&old);
}

// 每个 tick 合并群系刷新；取消或加载失败前的已完成部分同样同步。
class BiomeUpdates {
public:
    explicit BiomeUpdates(Level& level) : mLevel(level) {}
    ~BiomeUpdates() {
        for (auto const& [dimension, chunks] : mChunks) refreshBiomes(mLevel, dimension, chunks);
    }
    void add(int dimension, core::Vec3i p) {
        auto& chunks = mChunks[dimension];
        ChunkPos chunk{BlockPos{p.x, p.y, p.z}};
        if (chunks.empty() || chunks.back() != chunk) chunks.push_back(chunk);
    }
private:
    Level& mLevel;
    std::map<int, std::vector<ChunkPos>> mChunks;
};

class BiomeEditJob final : public exec::Job {
public:
    BiomeEditJob(mce::UUID owner, int dimension, core::Box box, std::string biome)
    : mOwner(owner), mDimension(dimension), mBox(std::move(box)), mIterator(mBox),
      mBiome(std::move(biome)), mMask(globalMask(owner)), mChanges(dimension) {
        mChanges.label = "setbiome";
        mChunks.setNeighbors(maskNeighbors(mMask));
    }
    std::uint64_t total() const override { return mBox.volume(); }
    std::uint64_t progress() const override { return mVisited; }
    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) { mError = tr("job.no_dimension"); return exec::JobState::failed; }
        auto* biome = findBiome(level, mBiome);
        if (!biome) { mError = tr("biome.unknown", mBiome); return exec::JobState::failed; }
        BiomeUpdates updates(level);
        mChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used)) {
            auto current = mIterator.current();
            if (!current) return exec::JobState::done;
            auto p = *current;
            if (mIterator.includes(p)) {
                auto state = mChunks.prepare(level, *region, mOwner, p);
                if (state == ChunkAccess::State::loading) return exec::JobState::running;
                if (state == ChunkAccess::State::failed) { mError = chunkError(p); return exec::JobState::failed; }
                if ((!mMask || mMask->test(region->getBlock({p.x,p.y,p.z}), *region, p))
                    && writeBiome(*region, p, *biome, &mChanges)) {
                    ++mChanged;
                    updates.add(mDimension, p);
                }
            }
            mIterator.advance();
            ++mVisited;
            ++used;
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override {
        finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart);
    }
private:
    mce::UUID mOwner;
    int mDimension;
    core::Box mBox;
    core::BoxIterator mIterator;
    std::string mBiome;
    std::optional<Mask> mMask;
    ChangeSet mChanges;
    ChunkAccess mChunks;
    std::uint64_t mVisited = 0, mChanged = 0;
    Clock::time_point mStart = Clock::now();
};

class RegionEditJob final : public exec::Job {
public:
    RegionEditJob(
        mce::UUID           owner,
        int                 dimension,
        core::Box           box,
        core::BoxShape      shape,
        Pattern             pattern,
        std::optional<Mask> mask,
        std::vector<CompoundTag> removedActors
    )
    : mGlobalMask(globalMask(owner)), mOwner(owner),
      mDimension(dimension),
      mTotal(core::shapeVolume(box, shape)),
      mBox(box),
      mPattern(std::move(pattern)),
      mMask(std::move(mask)),
      mIterator(box, shape),
      mChanges(dimension), mRemovedActors(std::move(removedActors)) { mChunks.setNeighbors(maskNeighbors(mMask)||maskNeighbors(mGlobalMask)); mChanges.label = shape == core::BoxShape::all ? "set/replace" : shape == core::BoxShape::walls ? "walls" : "faces"; }

    std::uint64_t total() const override { return mTotal; }
    std::uint64_t progress() const override { return mVisited; }

    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) {
            mError = tr("job.no_dimension");
            return exec::JobState::failed;
        }
        std::uint64_t used = 0;
        mChunks.beginTick();
        while (!budget.exhausted(used)) {
            auto* current = mIterator.current();
            if (!current) {
                if(mActorIndex>=mRemovedActors.size()) return exec::JobState::done;
                auto tag=mRemovedActors[mActorIndex++];
                if(tag.contains("UniqueID")) if(auto actor=level.fetchEntity(ActorUniqueID{tag["UniqueID"].get<std::int64_t>()},false)) {
                    if(!actor->isPlayer()) {
                        auto pos=actor->getPosition();
                        mChanges.recordActor({int(std::floor(pos.x)),int(std::floor(pos.y)),int(std::floor(pos.z))},actor->getOrCreateUniqueID().rawID,tag,false);
                        actor->remove();
                    }
                }
                ++used;
                continue;
            }
            auto p = *current;
            if(!mIterator.includes(p)) {mIterator.advance();++mVisited;++used;continue;}
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) {
                mError = chunkError(p);
                return exec::JobState::failed;
            }
            auto const& oldBlock = region->getBlock(BlockPos{p.x, p.y, p.z});
            if (!mMask || mMask->test(oldBlock,*region,p)) {
                if (writePattern(mGlobalMask,*region,p,mPattern,mBox,mRandom,mChanges)) ++mChanged;
            }
            mIterator.advance();
            ++mVisited;
            ++used;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart);
    }

private:
    std::optional<Mask> mGlobalMask;
    mce::UUID           mOwner;
    int                 mDimension;
    std::uint64_t       mTotal;
    core::Box           mBox;
    Pattern             mPattern;
    std::optional<Mask> mMask;
    core::BoxIterator   mIterator;
    ChunkAccess         mChunks;
    ChangeSet           mChanges;
    std::mt19937        mRandom{std::random_device{}()};
    std::uint64_t       mVisited = 0, mChanged = 0;
    Clock::time_point   mStart   = Clock::now();
    std::vector<CompoundTag> mRemovedActors;
    size_t mActorIndex=0;
};

class ComputedJob final : public exec::Job {
public:
    ComputedJob(mce::UUID owner, int dimension, core::Box box, Pattern pattern, EditFilter filter, std::string label, bool neighbors)
    : mGlobalMask(globalMask(owner)), mOwner(owner), mDimension(dimension), mBox(box), mIterator(box), mPattern(std::move(pattern)), mFilter(std::move(filter)), mChanges(dimension) { mChunks.setNeighbors(neighbors||maskNeighbors(mGlobalMask)); mChanges.label = std::move(label); }
    std::uint64_t total() const override { return mBox.volume(); }
    std::uint64_t progress() const override { return mVisited; }
    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) { mError = tr("job.no_dimension"); return exec::JobState::failed; }
        mChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used)) {
            auto current = mIterator.current();
            if (!current) return exec::JobState::done;
            auto p = *current;
            if(!mIterator.includes(p)) {mIterator.advance();++mVisited;++used;continue;}
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) { mError = chunkError(p); return exec::JobState::failed; }
            if (mFilter(*region, p) && writePattern(mGlobalMask,*region,p,mPattern,mBox,mRandom,mChanges)) ++mChanged;
            mIterator.advance(); ++mVisited; ++used;
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override { finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart); }
private:
    std::optional<Mask> mGlobalMask;
    mce::UUID mOwner;
    int mDimension;
    core::Box mBox;
    core::BoxIterator mIterator;
    Pattern mPattern;
    EditFilter mFilter;
    ChangeSet mChanges;
    ChunkAccess mChunks;
    std::mt19937 mRandom{std::random_device{}()};
    std::uint64_t mVisited = 0, mChanged = 0;
    Clock::time_point mStart = Clock::now();
};

class TerrainBrushJob final : public exec::Job {
public:
    TerrainBrushJob(
        mce::UUID owner, int dimension, core::Vec3i center, core::Box box, int radius, int iterations,
        std::optional<Mask> mask
    )
    : mGlobalMask(globalMask(owner)), mOwner(owner), mDimension(dimension), mCenter(center), mBox(box), mWidth(box.size().x),
      mColumns(mWidth * box.size().z), mRadius(radius), mIterations(iterations), mMask(std::move(mask)),
      mChanges(dimension), mHeights(iterations ? mColumns : 0), mSurface(mHeights), mNext(mHeights) {
        mChunks.setNeighbors(maskNeighbors(mMask)||maskNeighbors(mGlobalMask));
        mChanges.label = iterations ? "brush smooth" : "brush gravity";
    }

    std::uint64_t total() const override { return mColumns * (mIterations ? mIterations + 2 : 1); }
    std::uint64_t progress() const override { return mPhase * mColumns + mColumn; }

    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) {
            mError = tr("job.no_dimension");
            return exec::JobState::failed;
        }

        mChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used, 0)) {
            if (mColumn == mColumns) {
                if (progress() == total()) return exec::JobState::done;
                if (mPhase > 0) mHeights.swap(mNext);
                mColumn = 0;
                ++mPhase;
            }
            core::Vec3i p{mBox.min.x + mColumn % mWidth, mBox.max.y, mBox.min.z + mColumn / mWidth};
            auto dx = p.x - mCenter.x;
            auto dz = p.z - mCenter.z;
            bool inside = dx * dx + dz * dz <= mRadius * mRadius;

            if (mIterations && mPhase > 0 && mPhase <= mIterations) {
                mNext[mColumn] = inside ? averageHeight() : mHeights[mColumn];
                ++used;
            } else if (inside || (mIterations && mPhase == 0)) {
                auto state = mChunks.prepare(level, *region, mOwner, p);
                if (state == ChunkAccess::State::loading) return exec::JobState::running;
                if (state == ChunkAccess::State::failed) {
                    mError = chunkError(p);
                    return exec::JobState::failed;
                }
                if (!mIterations) {
                    used += applyGravity(*region, p);
                } else {
                    if (mPhase == 0) mSurface[mColumn] = mHeights[mColumn] = findSurface(*region, p);
                    else applySmoothing(*region, p);
                    used += mBox.size().y;
                }
            } else {
                ++used;
            }
            ++mColumn;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart);
    }

private:
    bool matches(Block const& block, BlockSource& region, core::Vec3i p) const { return !mMask || mMask->test(block,region,p); }

    static bool terrainBlock(BlockSource& region, BlockPos const& pos, Block const& block) {
        if (!block._isSolid() || isDisallowed(block)
            || block.getBlockType().mBlockEntityType != BlockActorType::Undefined
            || !region.getExtraBlock(pos).isAir()) return false;
        auto const& tag = *block.mSerializationId;
        if (!tag.contains("states")) return true;
        auto const& states = tag["states"].get<CompoundTag>();
        return !states.contains("upper_block_bit") && !states.contains("head_piece_bit");
    }

    int findSurface(BlockSource& region, core::Vec3i p) const {
        for (; p.y >= mBox.min.y; --p.y) {
            if (region.getBlock({p.x, p.y, p.z})._isSolid()) return p.y;
        }
        return mBox.min.y - 1;
    }

    double averageHeight() const {
        auto height = mHeights[mColumn];
        if (height < mBox.min.y) return height;
        double sum = height;
        int count = 1;
        auto include = [&](int index) {
            if (mHeights[index] >= mBox.min.y) {
                sum += mHeights[index];
                ++count;
            }
        };
        if (mColumn % mWidth > 0) include(mColumn - 1);
        if (mColumn % mWidth + 1 < mWidth) include(mColumn + 1);
        if (mColumn >= mWidth) include(mColumn - mWidth);
        if (mColumn + mWidth < mColumns) include(mColumn + mWidth);
        return sum / count;
    }

    void applySmoothing(BlockSource& region, core::Vec3i p) {
        int surface = int(mSurface[mColumn]);
        int target = int(std::lround(mHeights[mColumn]));
        if (surface == target || findSurface(region, p) != surface) return;

        p.y = surface;
        auto const& top = region.getBlock({p.x, p.y, p.z});
        if (!terrainBlock(region, {p.x, p.y, p.z}, top)) return;
        if (surface < target) {
            for (++p.y; p.y <= target; ++p.y) {
                BlockPos pos{p.x, p.y, p.z};
                auto const& block = region.getBlock(pos);
                if (!block.isAir() || !region.getExtraBlock(pos).isAir() || !matches(block,region,p)) break;
                if (writeMasked(mGlobalMask, region, p, top, airBlock(), nullptr, &mChanges)) ++mChanged;
            }
        } else {
            for (; p.y > target; --p.y) {
                BlockPos pos{p.x, p.y, p.z};
                auto const& block = region.getBlock(pos);
                if (!terrainBlock(region, pos, block) || !matches(block,region,p)) break;
                if (writeMasked(mGlobalMask, region, p, airBlock(), airBlock(), nullptr, &mChanges)) ++mChanged;
            }
        }
    }

    int applyGravity(BlockSource& region, core::Vec3i p) {
        int minY = region.getMinHeight();
        int height = mBox.max.y - minY + 1;
        mColumnBlocks.resize(height);
        int landing = 0;
        for (int i = 0; i < height; ++i) {
            BlockPos pos{p.x, minY + i, p.z};
            auto const& block = region.getBlock(pos);
            mColumnBlocks[i] = &block;
            if (block.isAir() && region.getExtraBlock(pos).isAir()) continue;
            if (pos.y >= mBox.min.y && landing < i && matches(block,region,{pos.x,pos.y,pos.z}) && terrainBlock(region, pos, block)) {
                mColumnBlocks[landing++] = &block;
                mColumnBlocks[i] = &airBlock();
            } else {
                landing = i + 1;
            }
        }
        for (int i = 0; i < height; ++i) {
            p.y = minY + i;
            auto const& old = region.getBlock({p.x, p.y, p.z});
            if (&old != mColumnBlocks[i] && writeMasked(mGlobalMask, region, p, *mColumnBlocks[i], airBlock(), nullptr, &mChanges, &old)) {
                ++mChanged;
            }
        }
        return height * 2;
    }

    std::optional<Mask> mGlobalMask;
    mce::UUID           mOwner;
    int                 mDimension;
    core::Vec3i         mCenter;
    core::Box           mBox;
    int                 mWidth, mColumns, mRadius, mIterations;
    std::optional<Mask> mMask;
    ChangeSet           mChanges;
    ChunkAccess         mChunks;
    std::vector<double> mHeights, mSurface, mNext;
    std::vector<Block const*> mColumnBlocks;
    int                 mPhase = 0, mColumn = 0;
    std::uint64_t       mChanged = 0;
    Clock::time_point   mStart = Clock::now();
};

class InspectJob final : public exec::Job {
public:
    InspectJob(mce::UUID owner, int dimension, core::Box box, std::optional<Mask> mask)
    : mOwner(owner), mDimension(dimension), mBox(box), mIterator(box), mMask(std::move(mask)) {mChunks.setNeighbors(maskNeighbors(mMask));}
    InspectJob(mce::UUID owner,std::shared_ptr<Clipboard const> clipboard)
    : InspectJob(owner,0,{{0,0,0},clipboard->size-core::Vec3i{1,1,1}},std::nullopt) {mClipboard=std::move(clipboard);}
    std::uint64_t total() const override { return mBox.volume(); }
    std::uint64_t progress() const override { return mVisited; }
    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) { mError = tr("job.no_dimension"); return exec::JobState::failed; }
        mChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used)) {
            auto current = mIterator.current();
            if (!current) return exec::JobState::done;
            auto p = *current;
            if(!mIterator.includes(p)) {mIterator.advance();++mVisited;++used;continue;}
            Block const* block=nullptr;
            if(mClipboard) block=mClipboard->at(mClipboard->index(p)).block;
            else {
                auto state = mChunks.prepare(level, *region, mOwner, p);
                if (state == ChunkAccess::State::loading) return exec::JobState::running;
                if (state == ChunkAccess::State::failed) { mError = chunkError(p); return exec::JobState::failed; }
                block=&region->getBlock({p.x,p.y,p.z});
            }
            if (block && (!mMask || mMask->test(*block,*region,p))) { ++mMatched; ++mCounts[block->getTypeName()]; }
            mIterator.advance(); ++mVisited; ++used;
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override {
        if (end != exec::JobEnd::completed) { tell(player, mError.empty() ? std::string(tr("inspect.cancelled")) : mError); return; }
        if (mMask) { tell(player, tr("inspect.count", mMatched)); return; }
        std::vector<std::pair<std::string, std::uint64_t>> values(mCounts.begin(), mCounts.end());
        std::ranges::sort(values, [](auto const& a, auto const& b) {
            return a.second == b.second ? a.first < b.first : a.second > b.second;
        });
        constexpr size_t maxRows = 12;
        std::string text = tr("inspect.count", mMatched) + "\n";
        for (size_t i = 0; i < std::min(values.size(), maxRows); ++i) {
            auto const& [name, count] = values[i];
            text += fmt::format("{}: {} ({:.1f}%)\n", name, count, 100.0 * double(count) / double(mMatched));
        }
        if (values.size() > maxRows) text += tr("inspect.more", values.size() - maxRows);
        tell(player, text);
    }
private:
    mce::UUID mOwner;
    int mDimension;
    core::Box mBox;
    core::BoxIterator mIterator;
    std::optional<Mask> mMask;
    ChunkAccess mChunks;
    std::uint64_t mVisited = 0, mMatched = 0;
    std::unordered_map<std::string, std::uint64_t> mCounts;
    std::shared_ptr<Clipboard const> mClipboard;
};

class TransferJob final : public exec::Job {
public:
    TransferJob(
        mce::UUID owner, int dimension, core::Box source, core::Vec3i offset, int copies,
        bool move, bool skipAir, std::optional<Mask> mask, bool includeEntities
    )
    : mGlobalMask(globalMask(owner)), mOwner(owner), mDimension(dimension), mSource(source), mOffset(offset), mCopies(copies), mMove(move), mSkipAir(skipAir),
      mMask(std::move(mask)), mIncludeEntities(includeEntities), mIterator(source), mChanges(dimension) {
        mChunks.setNeighbors(maskNeighbors(mMask)||maskNeighbors(mGlobalMask));
        mData.size = source.size();
        mChanges.label = move ? "move" : "stack";
    }
    std::uint64_t total() const override { return mSource.volume() * (1 + mCopies + (mMove ? 1 : 0)); }
    std::uint64_t progress() const override { return mVisited; }
    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) { mError = tr("job.no_dimension"); return exec::JobState::failed; }
        mChunks.beginTick();
        mActorSourceChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used)) {
            if (mStage == Stage::actors) {
                if (mActors.empty() || mCopy > mCopies) return exec::JobState::done;
                auto result = transferActor(level, *region);
                if (result != exec::JobState::done) return result;
                if (++mActorIndex == mActors.size()) {
                    mActorIndex = 0;
                    ++mCopy;
                }
                ++used;
                continue;
            }
            auto current = mIterator.current();
            if (!current) {
                if (mStage == Stage::read) {
                    if (mMove && isDisallowed(airBlock())) {
                        mError = tr("pattern.disallowed", airBlock().getTypeName());
                        return exec::JobState::failed;
                    }
                    mStage = mMove ? Stage::clear : Stage::paste;
                } else if (mStage == Stage::clear) {
                    mStage = Stage::paste;
                } else if (++mCopy > mCopies) {
                    mStage = Stage::actors;
                    mCopy = 1;
                }
                mIterator = core::BoxIterator(mSource);
                continue;
            }
            auto source = *current;
            if(!mIterator.includes(source)) {advance(used);continue;}
            auto index = mData.index(source - mSource.min);
            auto target = mStage == Stage::paste ? source + copyOffset() : source;
            Block const* block = nullptr;
            Block const* extra = nullptr;
            CompoundTag const* tag = nullptr;
            if (mStage != Stage::read) {
                auto const& cell = mData.at(index);
                if (cell.block && mStage == Stage::clear) {
                    auto incoming = source - mOffset;
                    // 只有快照里实际参与粘贴的方块才能替代这次清空。
                    if (!mSource.contains(incoming) || !mData.at(mData.index(incoming - mSource.min)).block) {
                        block = extra = &airBlock();
                    }
                } else if (cell.block && (mMove || !isDisallowed(*cell.block))) {
                    block = cell.block;
                    extra = cell.extra;
                    auto entity = mData.entities.find(index);
                    if (entity != mData.entities.end()) tag = &entity->second;
                }
                if (!block) {
                    advance(used);
                    continue;
                }
            }
            auto state = mChunks.prepare(level, *region, mOwner, target);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) { mError = chunkError(target); return exec::JobState::failed; }
            if (mStage == Stage::read) {
                if (mIncludeEntities) captureActors(*region, source);
                if (!captureBlock(*region, source, index)) return exec::JobState::failed;
            } else {
                if (writeMasked(mGlobalMask, *region, target, *block, *extra, tag, &mChanges)) ++mChanged;
            }
            advance(used);
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override { finishEdit(mOwner,player,end,mChanges,mChanged,mError,mStart); }
private:
    enum class Stage { read, clear, paste, actors };

    struct SavedActor {
        std::int64_t id;
        Vec3 position;
        CompoundTag tag;
    };

    core::Vec3i copyOffset() const { return {mOffset.x * mCopy, mOffset.y * mCopy, mOffset.z * mCopy}; }

    void advance(std::uint64_t& used) {
        mIterator.advance();
        ++mVisited;
        ++used;
    }

    bool captureBlock(BlockSource& region, core::Vec3i source, size_t index) {
        BlockPos pos{source.x, source.y, source.z};
        auto const& block = region.getBlock(pos);
        if ((mSkipAir && block.isAir()) || (mMask && !mMask->test(block,region,source))) {
            mData.store(index, {nullptr, nullptr});
            return true;
        }
        if (mMove && isDisallowed(block)) {
            mError = tr("pattern.disallowed", block.getTypeName());
            return false;
        }
        mData.store(index, {&block, &region.getExtraBlock(pos)});
        if (auto actor = region.getBlockEntity(pos)) {
            CompoundTag tag;
            if (actor->save(tag, SaveContext{})) mData.entities.emplace(index, std::move(tag));
        }
        return true;
    }

    static core::Vec3i blockPosition(Vec3 const& pos) {
        return {int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))};
    }

    void captureActors(BlockSource& region, core::Vec3i source) {
        auto chunk = std::pair{source.x >> 4, source.z >> 4};
        if (mActorChunk == chunk) return;
        mActorChunk = chunk;
        auto x = chunk.first * 16, z = chunk.second * 16;
        AABB bounds{
            {float(std::max(mSource.min.x, x)), float(mSource.min.y), float(std::max(mSource.min.z, z))},
            {float(std::min(mSource.max.x, x + 15) + 1), float(mSource.max.y + 1), float(std::min(mSource.max.z, z + 15) + 1)}
        };
        for (auto actor : region.getEntities(bounds, 0)) {
            if (actor->isPlayer()) continue;
            auto position = actor->getPosition();
            auto block = blockPosition(position);
            if (!mSource.contains(block) || (block.x >> 4) != chunk.first || (block.z >> 4) != chunk.second) continue;
            auto id = actor->getOrCreateUniqueID().rawID;
            if (!mActorIds.insert(id).second) continue;
            CompoundTag tag;
            if (actor->save(tag)) mActors.push_back({id, position, std::move(tag)});
        }
    }

    exec::JobState transferActor(Level& level, BlockSource& region) {
        auto const& entry = mActors[mActorIndex];
        auto source = blockPosition(entry.position);
        Actor* original = nullptr;
        if (mMove) {
            auto state = mActorSourceChunks.prepare(level, region, mOwner, source);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) { mError = chunkError(source); return exec::JobState::failed; }
            original = level.fetchEntity(ActorUniqueID{entry.id}, false);
            if (!original || original->getDimensionId() != mDimension) return exec::JobState::done;
        }
        auto offset = copyOffset();
        auto target = source + offset;
        auto state = mChunks.prepare(level, region, mOwner, target);
        if (state == ChunkAccess::State::loading) return exec::JobState::running;
        if (state == ChunkAccess::State::failed) { mError = chunkError(target); return exec::JobState::failed; }

        auto sourceTag = entry.tag;
        if (original) {
            CompoundTag current;
            if (original->save(current)) sourceTag = std::move(current);
        }
        auto tag = sourceTag;
        tag["Pos"] = storage::Nbt::array({entry.position.x + offset.x, entry.position.y + offset.y, entry.position.z + offset.z});
        tag.erase("UniqueID");
        auto actor = region.spawnActor(tag);
        if (!actor) { mError = tr("clipboard.entity_failed"); return exec::JobState::failed; }

        if (original) {
            mChanges.recordActor(blockPosition(original->getPosition()), entry.id, std::move(sourceTag), false);
            original->remove();
        }
        CompoundTag saved;
        if (!actor->save(saved)) saved = std::move(tag);
        mChanges.recordActor(target, actor->getOrCreateUniqueID().rawID, std::move(saved), true);
        return exec::JobState::done;
    }

    std::optional<Mask> mGlobalMask;
    mce::UUID mOwner;
    int mDimension;
    core::Box mSource;
    core::Vec3i mOffset;
    int mCopies, mCopy = 1;
    Stage mStage = Stage::read;
    bool mMove, mSkipAir;
    std::optional<Mask> mMask;
    bool mIncludeEntities;
    core::BoxIterator mIterator;
    Clipboard mData;
    std::optional<std::pair<int, int>> mActorChunk;
    std::set<std::int64_t> mActorIds;
    std::vector<SavedActor> mActors;
    size_t mActorIndex = 0;
    ChangeSet mChanges;
    ChunkAccess mChunks;
    ChunkAccess mActorSourceChunks;
    std::uint64_t mVisited = 0, mChanged = 0;
    Clock::time_point mStart = Clock::now();
};

class CopyJob final : public exec::Job {
public:
    CopyJob(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern,
            std::optional<Mask> mask, bool includeEntities, CopyDestination destination)
    : mOwner(owner),
      mDimension(dimension),
      mBox(box),
      mIterator(box),
      mCutPattern(std::move(cutPattern)),
      mClipboard(std::make_shared<Clipboard>()), mMask(std::move(mask)), mIncludeEntities(includeEntities),
      mDestination(std::move(destination)) {
        mChunks.setNeighbors(maskNeighbors(mMask));
        mClipboard->size   = box.size();
        mClipboard->offset = offset;
        mClipboard->origin = box.min;
    }

    std::uint64_t total() const override { return mBox.volume(); }
    std::uint64_t progress() const override { return mRead; }

    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) {
            mError = tr("job.no_dimension");
            return exec::JobState::failed;
        }
        // 读取比写入便宜得多，每 4 个方块计 1 份预算。
        std::uint64_t used = 0;
        mChunks.beginTick();
        while (!budget.exhausted(used)) {
            auto* current = mIterator.current();
            if (!current) return exec::JobState::done;
            auto p     = *current;
            if(!mIterator.includes(p)) {mIterator.advance();++mRead;++used;continue;}
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) {
                mError = chunkError(p);
                return exec::JobState::failed;
            }
            mIterator.advance();
            if(mIncludeEntities && mActorChunks.emplace(p.x>>4,p.z>>4).second) {
                auto minX=std::max(mBox.min.x,(p.x>>4)*16),maxX=std::min(mBox.max.x,(p.x>>4)*16+15);
                auto minZ=std::max(mBox.min.z,(p.z>>4)*16),maxZ=std::min(mBox.max.z,(p.z>>4)*16+15);
                AABB bounds{{float(minX),float(mBox.min.y),float(minZ)},{float(maxX+1),float(mBox.max.y+1),float(maxZ+1)}};
                for(auto actor:region->getEntities(bounds,0)) {
                    if(actor->isPlayer() || !bounds.contains(actor->getPosition())) continue;
                    if(!mActorIds.insert(actor->getOrCreateUniqueID().rawID).second) continue;
                    CompoundTag tag;
                    if(!actor->save(tag)) continue;
                    auto pos=actor->getPosition();
                    if(!mBox.contains({int(std::floor(pos.x)),int(std::floor(pos.y)),int(std::floor(pos.z))})) continue;
                    CompoundTag value;
                    value["pos"]=storage::Nbt::array({double(pos.x-mBox.min.x),double(pos.y-mBox.min.y),double(pos.z-mBox.min.z)});
                    value["blockPos"]=storage::vector({int(std::floor(pos.x))-mBox.min.x,int(std::floor(pos.y))-mBox.min.y,int(std::floor(pos.z))-mBox.min.z});
                    value["nbt"]=std::move(tag);
                    mClipboard->actors.push_back(std::move(value));
                }
            }
            if ((++mRead & 3) == 0) ++used;
            BlockPos    pos{p.x, p.y, p.z};
            auto        index        = mClipboard->index(p - mBox.min);
            auto const& block        = region->getBlock(pos);
            if(mMask && !mMask->test(block,*region,p)) {mClipboard->store(index,{nullptr,nullptr});continue;}
            mClipboard->store(index, {&block, &region->getExtraBlock(pos)});
            if (block.getBlockType().mBlockEntityType == BlockActorType::Undefined) continue;
            if (auto* actor = region->getBlockEntity(pos)) {
                CompoundTag tag;
                if (actor->save(tag, SaveContext{})) mClipboard->entities.emplace(index, std::move(tag));
            }
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        if (end != exec::JobEnd::completed) {
            tell(player, end == exec::JobEnd::failed ? mError : std::string(tr("clipboard.copy_cancelled")));
            return;
        }
        mCompleted = true;
        if (mDestination) return;
        if (auto* session = findSession(mOwner)) session->clipboard = mClipboard;
        saveClipboard(mOwner);
        auto size = mBox.size();
        tell(player, tr(mCutPattern ? "clipboard.cut_copied" : "clipboard.copied", size.x, size.y, size.z));
    }

    std::unique_ptr<exec::Job> next() override {
        if (!mCompleted) return nullptr;
        if (mDestination) return mDestination(std::move(mClipboard));
        if (!mCutPattern || !findSession(mOwner)) return nullptr;
        std::vector<CompoundTag> removed;
        if(mIncludeEntities) for(auto const& actor:mClipboard->actors) removed.push_back(actor["nbt"].get<CompoundTag>());
        return makeRegionEdit(mOwner, mDimension, mBox, core::BoxShape::all, std::move(*mCutPattern), std::move(mMask),std::move(removed));
    }

private:
    mce::UUID                  mOwner;
    int                        mDimension;
    core::Box                  mBox;
    core::BoxIterator          mIterator;
    ChunkAccess                mChunks;
    std::optional<Pattern>     mCutPattern;
    std::shared_ptr<Clipboard> mClipboard;
    std::uint64_t              mRead      = 0;
    bool                       mCompleted = false;
    std::optional<Mask> mMask;
    bool mIncludeEntities;
    CopyDestination mDestination;
    std::set<std::pair<int,int>> mActorChunks;
    std::set<std::int64_t> mActorIds;
};

class PasteJob final : public exec::Job {
public:
    PasteJob(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir, std::optional<Mask> mask, bool includeEntities, bool maskTarget)
    : mGlobalMask(globalMask(owner)), mOwner(owner),
      mDimension(dimension),
      mClipboard(std::move(clipboard)),
      mBox{min, min + mClipboard->size - core::Vec3i{1, 1, 1}},
      mSkipAir(skipAir),
      mIterator(mBox),
      mChanges(dimension), mMask(std::move(mask)), mIncludeEntities(includeEntities), mMaskTarget(maskTarget) {
        mChunks.setNeighbors(maskNeighbors(mMask)||maskNeighbors(mGlobalMask));
        mChanges.label = maskTarget ? "brush clipboard" : "paste";
    }

    std::uint64_t total() const override { return mBox.volume(); }
    std::uint64_t progress() const override { return mVisited; }

    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) {
            mError = tr("job.no_dimension");
            return exec::JobState::failed;
        }
        std::uint64_t used = 0;
        auto const    startVisited = mVisited;
        mChunks.beginTick();
        while (!budget.exhausted(used, mVisited - startVisited)) {
            auto* current = mIterator.current();
            if (!current) {
                if(!mIncludeEntities || mActorIndex>=mClipboard->actors.size()) return exec::JobState::done;
                auto const& entry=mClipboard->actors[mActorIndex];
                auto const& local=entry["pos"].get<ListTag>();
                double x=double(local[0])+mBox.min.x,y=double(local[1])+mBox.min.y,z=double(local[2])+mBox.min.z;
                core::Vec3i pos{int(std::floor(x)),int(std::floor(y)),int(std::floor(z))};
                auto state=mChunks.prepare(level,*region,mOwner,pos);
                if(state==ChunkAccess::State::loading) return exec::JobState::running;
                if(state==ChunkAccess::State::failed) {mError=chunkError(pos);return exec::JobState::failed;}
                auto tag=entry["nbt"].get<CompoundTag>();
                tag["Pos"]=storage::Nbt::array({float(x),float(y),float(z)});
                tag.erase("UniqueID");
                auto actor=region->spawnActor(tag);
                if(!actor) {mError=tr("clipboard.entity_failed");return exec::JobState::failed;}
                CompoundTag saved;
                if(!actor->save(saved)) saved=tag;
                mChanges.recordActor(pos,actor->getOrCreateUniqueID().rawID,std::move(saved),true);
                ++mActorIndex; ++used;
                continue;
            }
            auto  p     = *current;
            auto  index = mClipboard->index(p - mBox.min);
            auto [block, extra] = mClipboard->at(index);
            if (block && (!mSkipAir || !block->isAir())) {
                ++used;
                if (isDisallowed(*block)) {
                    ++mDenied;
                } else {
                    auto state = mChunks.prepare(level, *region, mOwner, p);
                    if (state == ChunkAccess::State::loading) return exec::JobState::running;
                    if (state == ChunkAccess::State::failed) {
                        mError = chunkError(p);
                        return exec::JobState::failed;
                    }
                    auto               entity = mClipboard->entities.find(index);
                    CompoundTag const* tag    = entity == mClipboard->entities.end() ? nullptr : &entity->second;
                    if ((!mMask || mMask->test(mMaskTarget ? region->getBlock({p.x,p.y,p.z}) : *block,*region,p))
                        && writeMasked(mGlobalMask, *region, p, *block, extra ? *extra : airBlock(), tag, &mChanges)) ++mChanged;
                }
            }
            mIterator.advance();
            ++mVisited;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart);
        if (mDenied) tell(player, tr("clipboard.denied_blocks", mDenied));
    }

private:
    std::optional<Mask> mGlobalMask;
    mce::UUID                        mOwner;
    int                              mDimension;
    std::shared_ptr<Clipboard const> mClipboard;
    core::Box                        mBox;
    bool                             mSkipAir;
    core::BoxIterator                mIterator;
    ChunkAccess                      mChunks;
    ChangeSet                        mChanges;
    std::uint64_t                    mVisited = 0, mChanged = 0, mDenied = 0;
    Clock::time_point                mStart   = Clock::now();
    std::optional<Mask> mMask;
    bool mIncludeEntities;
    bool mMaskTarget;
    size_t mActorIndex=0;
};

class HistoryJob final : public exec::Job {
public:
    HistoryJob(mce::UUID owner, bool undo, std::vector<ChangeSet> sets, mce::UUID requester)
    : mOwner(owner),
      mUndo(undo),
      mSets(std::move(sets)), mRequester(requester) {
        for (auto const& changes : mSets) mTotal += changes.size();
    }

    std::uint64_t total() const override { return mTotal; }
    std::uint64_t progress() const override { return mDone; }

    exec::JobState run(Level& level, exec::Budget const& budget) override {
        BiomeUpdates updates(level);
        std::uint64_t used = 0;
        mChunks.beginTick();
        while (!budget.exhausted(used)) {
            if (mSet >= mSets.size()) return exec::JobState::done;
            auto& changes = mSets[mSet];
            try {
                if (mApplied >= changes.size()) {
                    if (mReleaseAfterSet && !changes.trySpill()) return exec::JobState::running;
                    ++mSet;
                    mApplied = 0;
                    mReleaseAfterSet = false;
                    mChunks = {};
                    continue;
                }
                if (changes.residentSize() == 0) {
                    changes.ensureLoaded();
                    mReleaseAfterSet = true;
                }
            } catch (std::exception const& error) {
                mError = tr("storage.failed", error.what());
                return exec::JobState::failed;
            }
            auto* region = exec::regionOf(level, changes.dimension());
            if (!region) {
                mError = tr("job.no_dimension");
                return exec::JobState::failed;
            }
            // 撤销从后往前写回旧值，重做从前往后写回新值。
            auto index = mUndo ? changes.size() - 1 - mApplied : mApplied;
            auto p     = changes.position(index);
            auto state = mChunks.prepare(level, *region, mRequester, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) {
                mError = chunkError(p);
                return exec::JobState::failed;
            }
            try {
                changes.apply(*region, index, mUndo);
                if (changes.isBiome(index)) updates.add(changes.dimension(), p);
            }
            catch(std::exception const& error) {mError=tr("advanced.failed",error.what());return exec::JobState::failed;}
            ++mApplied;
            ++mDone;
            ++used;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        if (auto* session = findSession(mOwner)) returnToHistory(session->history);
        if(mOwner!=mRequester) {
            auto level = ll::service::getLevel();
            if (level && level->getPlayer(mOwner)) saveSession(mOwner);
            else closeSession(mOwner);
        } else saveSession(mOwner);
        switch (end) {
        case exec::JobEnd::completed:
            tell(player, tr(mUndo ? "history.undone" : "history.redone", mSets.size(), mDone));
            break;
        case exec::JobEnd::cancelled:
            tell(player, tr("history.cancelled", mDone));
            break;
        case exec::JobEnd::failed:
            tell(player, mError);
            tell(player, tr("history.cancelled", mDone));
            break;
        }
    }

private:
    // 已写回的部分进入另一侧的栈，其余按原顺序退回原来的栈。
    void returnToHistory(History& history) {
        auto toTarget = [&](ChangeSet changes) {
            mUndo ? history.pushRedo(std::move(changes)) : history.pushUndo(std::move(changes));
        };
        auto toSource = [&](ChangeSet changes) {
            mUndo ? history.pushUndo(std::move(changes)) : history.pushRedo(std::move(changes));
        };
        for (size_t i = 0; i < mSet && i < mSets.size(); ++i) toTarget(std::move(mSets[i]));
        if (mSet >= mSets.size()) return;

        auto& current = mSets[mSet];
        if(mApplied==0) {
            for(size_t i=mSets.size();i>mSet;--i) toSource(std::move(mSets[i-1]));
            return;
        }
        if (mUndo) {
            toTarget(current.splitTail(current.size() - mApplied));
        } else {
            auto rest = current.splitTail(mApplied);
            toTarget(std::move(current));
            current = std::move(rest);
        }
        for (size_t i = mSets.size() - 1; i > mSet; --i) toSource(std::move(mSets[i]));
        toSource(std::move(current));
    }

    mce::UUID              mOwner;
    bool                   mUndo;
    std::vector<ChangeSet> mSets;
    mce::UUID mRequester;
    ChunkAccess            mChunks;
    std::size_t            mSet = 0, mApplied = 0;
    bool                   mReleaseAfterSet = false;
    std::uint64_t          mTotal = 0, mDone = 0;
};

} // namespace

std::unique_ptr<exec::Job> makeBiomeEdit(mce::UUID owner, int dimension, core::Box box, std::string biome) {
    return std::make_unique<BiomeEditJob>(owner, dimension, std::move(box), std::move(biome));
}

std::unique_ptr<exec::Job> makeRegionEdit(
    mce::UUID           owner,
    int                 dimension,
    core::Box           box,
    core::BoxShape      shape,
    Pattern             pattern,
    std::optional<Mask> mask,
    std::vector<CompoundTag> removedActors
) {
    return std::make_unique<RegionEditJob>(owner, dimension, box, shape, std::move(pattern), std::move(mask),std::move(removedActors));
}

std::unique_ptr<exec::Job>
makeCopy(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern,
         std::optional<Mask> mask, bool includeEntities, CopyDestination destination) {
    return std::make_unique<CopyJob>(
        owner, dimension, box, offset, std::move(cutPattern), std::move(mask), includeEntities, std::move(destination)
    );
}

std::unique_ptr<exec::Job>
makePaste(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir, std::optional<Mask> mask, bool includeEntities, bool maskTarget) {
    return std::make_unique<PasteJob>(owner, dimension, std::move(clipboard), min, skipAir, std::move(mask), includeEntities, maskTarget);
}

std::unique_ptr<exec::Job> makeTerrainBrush(
    mce::UUID owner, int dimension, core::Vec3i center, core::Box box, int radius, int iterations, std::optional<Mask> mask
) {
    return std::make_unique<TerrainBrushJob>(owner, dimension, center, box, radius, iterations, std::move(mask));
}

std::unique_ptr<exec::Job> makeHistory(mce::UUID owner, bool undo, std::vector<ChangeSet> sets, mce::UUID requester) {
    return std::make_unique<HistoryJob>(owner, undo, std::move(sets),requester);
}

std::unique_ptr<exec::Job> makeComputedEdit(mce::UUID owner, int dimension, core::Box box, Pattern pattern, EditFilter filter, std::string label, bool neighbors) {
    return std::make_unique<ComputedJob>(owner, dimension, box, std::move(pattern), std::move(filter), std::move(label),neighbors);
}
std::unique_ptr<exec::Job> makeInspect(mce::UUID owner, int dimension, core::Box box, std::optional<Mask> mask) {
    return std::make_unique<InspectJob>(owner, dimension, box, std::move(mask));
}
std::unique_ptr<exec::Job> makeClipboardInspect(mce::UUID owner,std::shared_ptr<Clipboard const> clipboard) {
    return std::make_unique<InspectJob>(owner,std::move(clipboard));
}
std::unique_ptr<exec::Job> makeTransfer(
    mce::UUID owner, int dimension, core::Box source, core::Vec3i offset, int copies,
    bool move, bool skipAir, std::optional<Mask> mask, bool includeEntities
) {
    return std::make_unique<TransferJob>(owner, dimension, source, offset, copies, move, skipAir, std::move(mask), includeEntities);
}

} // namespace creatorx::edit
