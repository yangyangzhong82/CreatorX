#include "edit/Jobs.h"

#include "mod/Global.h"
#include "session/PlayerSession.h"

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
        std::optional<Mask> mask
    )
    : mOwner(owner),
      mDimension(dimension),
      mTotal(core::shapeVolume(box, shape)),
      mPattern(std::move(pattern)),
      mMask(std::move(mask)),
      mIterator(box, shape),
      mChanges(dimension) {}

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
            if (!current) return exec::JobState::done;
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
};

class CopyJob final : public exec::Job {
public:
    CopyJob(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern)
    : mOwner(owner),
      mDimension(dimension),
      mBox(box),
      mIterator(box),
      mCutPattern(std::move(cutPattern)),
      mClipboard(std::make_shared<Clipboard>()) {
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
            if ((++mRead & 3) == 0) ++used;
            BlockPos    pos{p.x, p.y, p.z};
            auto        index        = mClipboard->index(p - mBox.min);
            auto const& block        = region->getBlock(pos);
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
        auto size = mBox.size();
        tell(player, tr(mCutPattern ? "clipboard.cut_copied" : "clipboard.copied", size.x, size.y, size.z));
    }

    std::unique_ptr<exec::Job> next() override {
        if (!mCompleted || !mCutPattern || !findSession(mOwner)) return nullptr;
        return makeRegionEdit(mOwner, mDimension, mBox, core::BoxShape::all, std::move(*mCutPattern), std::nullopt);
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
};

class PasteJob final : public exec::Job {
public:
    PasteJob(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir)
    : mOwner(owner),
      mDimension(dimension),
      mClipboard(std::move(clipboard)),
      mBox{min, min + mClipboard->size - core::Vec3i{1, 1, 1}},
      mSkipAir(skipAir),
      mIterator(mBox),
      mChanges(dimension) {}

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
            if (!current) return exec::JobState::done;
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
};

class HistoryJob final : public exec::Job {
public:
    HistoryJob(mce::UUID owner, bool undo, std::vector<ChangeSet> sets)
    : mOwner(owner),
      mUndo(undo),
      mSets(std::move(sets)) {
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
            if (mApplied >= changes.size()) {
                ++mSet;
                mApplied = 0;
                mChunks  = {};
                continue;
            }
            auto* region = exec::regionOf(level, changes.dimension());
            if (!region) {
                mError = tr("job.no_dimension");
                return exec::JobState::failed;
            }
            // 撤销从后往前写回旧值，重做从前往后写回新值。
            auto index = mUndo ? changes.size() - 1 - mApplied : mApplied;
            auto p     = changes.position(index);
            auto state = mChunks.prepare(level, *region, mOwner, p);
            if (state == ChunkAccess::State::loading) return exec::JobState::running;
            if (state == ChunkAccess::State::failed) {
                mError = chunkError(p);
                return exec::JobState::failed;
            }
            changes.apply(*region, index, mUndo);
            ++mApplied;
            ++mDone;
            ++used;
        }
        return exec::JobState::running;
    }

    void finish(Player* player, exec::JobEnd end) override {
        if (auto* session = findSession(mOwner)) returnToHistory(session->history);
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
    ChunkAccess            mChunks;
    std::size_t            mSet = 0, mApplied = 0;
    std::uint64_t          mTotal = 0, mDone = 0;
};

} // namespace

std::unique_ptr<exec::Job> makeRegionEdit(
    mce::UUID           owner,
    int                 dimension,
    core::Box           box,
    core::BoxShape      shape,
    Pattern             pattern,
    std::optional<Mask> mask
) {
    return std::make_unique<RegionEditJob>(owner, dimension, box, shape, std::move(pattern), std::move(mask));
}

std::unique_ptr<exec::Job>
makeCopy(mce::UUID owner, int dimension, core::Box box, core::Vec3i offset, std::optional<Pattern> cutPattern) {
    return std::make_unique<CopyJob>(owner, dimension, box, offset, std::move(cutPattern));
}

std::unique_ptr<exec::Job>
makePaste(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir) {
    return std::make_unique<PasteJob>(owner, dimension, std::move(clipboard), min, skipAir);
}

std::unique_ptr<exec::Job> makeHistory(mce::UUID owner, bool undo, std::vector<ChangeSet> sets) {
    return std::make_unique<HistoryJob>(owner, undo, std::move(sets));
}

} // namespace creatorx::edit
