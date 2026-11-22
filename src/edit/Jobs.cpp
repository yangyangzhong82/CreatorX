#include "edit/Jobs.h"

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

    State prepare(Level& level, BlockSource& region, mce::UUID const& owner, core::Vec3i p) {
        auto chunk = ChunkPos(BlockPos{p.x, p.y, p.z});
        if (mReady == chunk) return State::ready;
        if (mRequested != chunk) {
            mView.reset();
            mRequested.reset();
        }

        auto& source = region.getChunkSource();
        if (source.getAvailableChunk(chunk)) {
            mReady = chunk;
            return State::ready;
        }
        if (mView) return Clock::now() < mDeadline ? State::loading : State::failed;
        if (!source.isWithinWorldLimit(chunk)) return State::failed;

        auto* player = level.getPlayer(owner);
        if (!player) return State::failed;
        mView = player->$_createChunkSource(source);
        if (!mView) return State::failed;

        auto x = chunk.x * 16;
        auto z = chunk.z * 16;
        mView->move(
            BlockPos{x, p.y, z},
            BlockPos{x + 15, p.y, z + 15},
            false,
            ChunkSourceViewGenerateMode::GenerateAll,
            [](auto) {},
            nullptr
        );
        mRequested = chunk;
        mDeadline  = Clock::now() + std::chrono::seconds(30);
        return State::loading;
    }

private:
    std::optional<ChunkPos>          mReady;
    std::optional<ChunkPos>          mRequested;
    std::shared_ptr<ChunkViewSource> mView;
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
    : mOwner(owner),
      mDimension(dimension),
      mTotal(core::shapeVolume(box, shape)),
      mPattern(std::move(pattern)),
      mMask(std::move(mask)),
      mIterator(box, shape),
      mChanges(dimension), mRemovedActors(std::move(removedActors)) { mChanges.label = shape == core::BoxShape::all ? "set/replace" : shape == core::BoxShape::walls ? "walls" : "faces"; }

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
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) {
                mError = chunkError(p);
                return exec::JobState::failed;
            }
            auto const& oldBlock = region->getBlock(BlockPos{p.x, p.y, p.z});
            if (!mMask || mMask->test(oldBlock)) {
                if (writeBlock(*region, p, mPattern.apply(mRandom), airBlock(), nullptr, &mChanges, &oldBlock)) ++mChanged;
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
    mce::UUID           mOwner;
    int                 mDimension;
    std::uint64_t       mTotal;
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
    ComputedJob(mce::UUID owner, int dimension, core::Box box, Pattern pattern, EditFilter filter, std::string label)
    : mOwner(owner), mDimension(dimension), mBox(box), mIterator(box), mPattern(std::move(pattern)), mFilter(std::move(filter)), mChanges(dimension) { mChanges.label = std::move(label); }
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
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) { mError = chunkError(p); return exec::JobState::failed; }
            if (mFilter(*region, p) && writeBlock(*region, p, mPattern.apply(mRandom), airBlock(), nullptr, &mChanges)) ++mChanged;
            mIterator.advance(); ++mVisited; ++used;
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override { finishEdit(mOwner, player, end, mChanges, mChanged, mError, mStart); }
private:
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

class InspectJob final : public exec::Job {
public:
    InspectJob(mce::UUID owner, int dimension, core::Box box, std::optional<Mask> mask)
    : mOwner(owner), mDimension(dimension), mBox(box), mIterator(box), mMask(std::move(mask)) {}
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
            Block const* block=nullptr;
            if(mClipboard) block=mClipboard->at(mClipboard->index(p)).block;
            else {
                auto state = mChunks.prepare(level, *region, mOwner, p);
                if (state == ChunkAccess::State::loading) return exec::JobState::running;
                if (state == ChunkAccess::State::failed) { mError = chunkError(p); return exec::JobState::failed; }
                block=&region->getBlock({p.x,p.y,p.z});
            }
            if (block && (!mMask || mMask->test(*block))) { ++mMatched; ++mCounts[block->getTypeName()]; }
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
    TransferJob(mce::UUID owner, int dimension, core::Box source, core::Vec3i offset, int copies, bool move, bool skipAir)
    : mOwner(owner), mDimension(dimension), mSource(source), mOffset(offset), mCopies(copies), mMove(move), mSkipAir(skipAir),
      mIterator(source), mChanges(dimension) {
        mData.size = source.size();
        mChanges.label = move ? "move" : "stack";
    }
    std::uint64_t total() const override { return mSource.volume() * (1 + mCopies + (mMove ? 1 : 0)); }
    std::uint64_t progress() const override { return mVisited; }
    exec::JobState run(Level& level, exec::Budget const& budget) override {
        auto* region = exec::regionOf(level, mDimension);
        if (!region) { mError = tr("job.no_dimension"); return exec::JobState::failed; }
        mChunks.beginTick();
        std::uint64_t used = 0;
        while (!budget.exhausted(used)) {
            auto current = mIterator.current();
            if (!current) {
                if (mStage == 0) {
                    if (mMove && isDisallowed(airBlock())) {
                        mError = tr("pattern.disallowed", airBlock().getTypeName());
                        return exec::JobState::failed;
                    }
                    mStage = mMove ? 1 : 2;
                    mIterator = core::BoxIterator(mSource);
                    continue;
                }
                if (mStage == 1) { mStage = 2; mIterator = core::BoxIterator(mSource); continue; }
                if (++mCopy > mCopies) return exec::JobState::done;
                mIterator = core::BoxIterator(mSource); continue;
            }
            auto source = *current;
            auto local = source - mSource.min;
            auto index = mData.index(local);
            auto target = mStage < 2 ? source : source + core::Vec3i{mOffset.x*mCopy,mOffset.y*mCopy,mOffset.z*mCopy};
            auto state = mChunks.prepare(level, *region, mOwner, target);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) { mError = chunkError(target); return exec::JobState::failed; }
            if (mStage == 0) {
                BlockPos pos{source.x,source.y,source.z};
                auto const& block = region->getBlock(pos);
                if (mMove && (!mSkipAir || !block.isAir()) && isDisallowed(block)) {
                    mError = tr("pattern.disallowed", block.getTypeName());
                    return exec::JobState::failed;
                }
                mData.store(index, {&block, &region->getExtraBlock(pos)});
                if (auto actor = region->getBlockEntity(pos)) {
                    CompoundTag tag;
                    if (actor->save(tag, SaveContext{})) mData.entities.emplace(index,std::move(tag));
                }
            } else {
                auto cell = mData.at(index);
                if (!mSkipAir || (cell.block && !cell.block->isAir())) {
                    auto block = mStage == 1 ? &airBlock() : cell.block;
                    auto extra = mStage == 1 ? &airBlock() : cell.extra;
                    if (block && (mMove || !isDisallowed(*block))) {
                        auto entity = mData.entities.find(index);
                        auto tag = mStage == 2 && entity != mData.entities.end() ? &entity->second : nullptr;
                        if (writeBlock(*region,target,*block,extra ? *extra : airBlock(),tag,&mChanges)) ++mChanged;
                    }
                }
            }
            mIterator.advance(); ++mVisited; ++used;
        }
        return exec::JobState::running;
    }
    void finish(Player* player, exec::JobEnd end) override { finishEdit(mOwner,player,end,mChanges,mChanged,mError,mStart); }
private:
    mce::UUID mOwner;
    int mDimension;
    core::Box mSource;
    core::Vec3i mOffset;
    int mCopies, mCopy = 1, mStage = 0;
    bool mMove, mSkipAir;
    core::BoxIterator mIterator;
    Clipboard mData;
    ChangeSet mChanges;
    ChunkAccess mChunks;
    std::uint64_t mVisited = 0, mChanged = 0;
    Clock::time_point mStart = Clock::now();
};

class CopyJob final : public exec::Job {
public:
    CopyJob(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern, std::optional<Mask> mask, bool includeEntities)
    : mOwner(owner),
      mDimension(dimension),
      mBox(box),
      mIterator(box),
      mCutPattern(std::move(cutPattern)),
      mClipboard(std::make_shared<Clipboard>()), mMask(std::move(mask)), mIncludeEntities(includeEntities) {
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
            if(mMask && !mMask->test(block)) {mClipboard->store(index,{nullptr,nullptr});continue;}
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
        if (auto* session = findSession(mOwner)) session->clipboard = mClipboard;
        saveClipboard(mOwner);
        auto size = mBox.size();
        tell(player, tr(mCutPattern ? "clipboard.cut_copied" : "clipboard.copied", size.x, size.y, size.z));
    }

    std::unique_ptr<exec::Job> next() override {
        if (!mCompleted || !mCutPattern || !findSession(mOwner)) return nullptr;
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
    std::set<std::pair<int,int>> mActorChunks;
    std::set<std::int64_t> mActorIds;
};

class PasteJob final : public exec::Job {
public:
    PasteJob(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir, std::optional<Mask> mask, bool includeEntities)
    : mOwner(owner),
      mDimension(dimension),
      mClipboard(std::move(clipboard)),
      mBox{min, min + mClipboard->size - core::Vec3i{1, 1, 1}},
      mSkipAir(skipAir),
      mIterator(mBox),
      mChanges(dimension), mMask(std::move(mask)), mIncludeEntities(includeEntities) {mChanges.label="paste";}

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
            if (block && (!mSkipAir || !block->isAir()) && (!mMask || mMask->test(*block))) {
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
                    if (writeBlock(*region, p, *block, extra ? *extra : airBlock(), tag, &mChanges)) ++mChanged;
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
            try {changes.apply(*region, index, mUndo);}
            catch(std::exception const& error) {mError=tr("advanced.failed",error.what());return exec::JobState::failed;}
            ++mApplied;
            ++mDone;
            ++used;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        if (auto* session = findSession(mOwner)) returnToHistory(session->history);
        saveSession(mOwner);
        if(mOwner!=mRequester) {
            if(auto level=ll::service::getLevel()) player=level->getPlayer(mRequester);
        }
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
makeCopy(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern, std::optional<Mask> mask, bool includeEntities) {
    return std::make_unique<CopyJob>(owner, dimension, box, offset, std::move(cutPattern),std::move(mask),includeEntities);
}

std::unique_ptr<exec::Job>
makePaste(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir, std::optional<Mask> mask, bool includeEntities) {
    return std::make_unique<PasteJob>(owner, dimension, std::move(clipboard), min, skipAir,std::move(mask),includeEntities);
}

std::unique_ptr<exec::Job> makeHistory(mce::UUID owner, bool undo, std::vector<ChangeSet> sets, mce::UUID requester) {
    return std::make_unique<HistoryJob>(owner, undo, std::move(sets),requester);
}

std::unique_ptr<exec::Job> makeComputedEdit(mce::UUID owner, int dimension, core::Box box, Pattern pattern, EditFilter filter, std::string label) {
    return std::make_unique<ComputedJob>(owner, dimension, box, std::move(pattern), std::move(filter), std::move(label));
}
std::unique_ptr<exec::Job> makeInspect(mce::UUID owner, int dimension, core::Box box, std::optional<Mask> mask) {
    return std::make_unique<InspectJob>(owner, dimension, box, std::move(mask));
}
std::unique_ptr<exec::Job> makeClipboardInspect(mce::UUID owner,std::shared_ptr<Clipboard const> clipboard) {
    return std::make_unique<InspectJob>(owner,std::move(clipboard));
}
std::unique_ptr<exec::Job> makeTransfer(mce::UUID owner, int dimension, core::Box source, core::Vec3i offset, int copies, bool move, bool skipAir) {
    return std::make_unique<TransferJob>(owner,dimension,source,offset,copies,move,skipAir);
}

} // namespace creatorx::edit
