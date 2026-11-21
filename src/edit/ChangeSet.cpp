#include "edit/ChangeSet.h"

#include "mod/Global.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/actor/BlockActor.h"

namespace creatorx::edit {
namespace {

// S1 未验证前的约定：1 = 通知邻居，2 = 同步客户端。关闭物理时只同步，不触发沙子下落、水流等更新。
int updateFlags() { return config.performance.physics ? 3 : 2; }

} // namespace

std::uint32_t ChangeSet::Pool::indexOf(Block const& block) {
    auto [it, inserted] = indices.try_emplace(&block, std::uint32_t(blocks.size()));
    if (inserted) blocks.push_back(&block);
    return it->second;
}

std::int32_t ChangeSet::Pool::addEntity(std::unique_ptr<CompoundTag> tag) {
    if (!tag) return -1;
    entities.push_back(std::move(tag));
    return std::int32_t(entities.size() - 1);
}

ChangeSet::ChangeSet(int dimension) : mDimension(dimension), mPool(std::make_shared<Pool>()) {}

void ChangeSet::record(
    core::Vec3i                  pos,
    Block const&                 oldBlock,
    Block const&                 oldExtra,
    std::unique_ptr<CompoundTag> oldEntity,
    Block const&                 newBlock,
    Block const&                 newExtra,
    CompoundTag const*           newEntity
) {
    mEntries.push_back({
        pos,
        mPool->indexOf(oldBlock),
        mPool->indexOf(oldExtra),
        mPool->indexOf(newBlock),
        mPool->indexOf(newExtra),
        mPool->addEntity(std::move(oldEntity)),
        mPool->addEntity(newEntity ? newEntity->clone() : nullptr),
    });
}

void ChangeSet::apply(BlockSource& region, size_t index, bool undo) const {
    auto const& entry  = mEntries[index];
    auto        block  = undo ? entry.oldBlock : entry.newBlock;
    auto        extra  = undo ? entry.oldExtra : entry.newExtra;
    auto        entity = undo ? entry.oldEntity : entry.newEntity;
    writeBlock(
        region,
        entry.pos,
        *mPool->blocks[block],
        *mPool->blocks[extra],
        entity < 0 ? nullptr : mPool->entities[entity].get(),
        nullptr
    );
}

ChangeSet ChangeSet::splitTail(size_t from) {
    ChangeSet tail(mDimension);
    tail.mPool = mPool;
    tail.mEntries.assign(mEntries.begin() + std::ptrdiff_t(from), mEntries.end());
    mEntries.resize(from);
    return tail;
}

bool writeBlock(
    BlockSource&       region,
    core::Vec3i        p,
    Block const&       block,
    Block const&       extra,
    CompoundTag const* entity,
    ChangeSet*         changes
) {
    BlockPos    pos{p.x, p.y, p.z};
    auto const& oldBlock = region.getBlock(pos);
    auto const& oldExtra = region.getExtraBlock(pos);
    if (&oldBlock == &block && &oldExtra == &extra && !entity) return false;

    if (changes) {
        std::unique_ptr<CompoundTag> oldEntity;
        if (auto* actor = region.getBlockEntity(pos)) {
            oldEntity = std::make_unique<CompoundTag>();
            if (!actor->save(*oldEntity, SaveContext{})) oldEntity.reset();
        }
        changes->record(p, oldBlock, oldExtra, std::move(oldEntity), block, extra, entity);
    }

    int                flags = updateFlags();
    BlockChangeContext context{};
    if (entity) {
        auto actor = BlockActor::create(*entity, pos);
        region.setBlock(pos, block, flags, actor, nullptr, context);
        if (actor) actor->refresh(region);
    } else if (&oldBlock != &block) {
        region.setBlock(pos, block, flags, nullptr, context);
    }
    // 换方块可能顺带清掉含水层，以写入后的实际状态为准。
    if (&region.getExtraBlock(pos) != &extra) region.setExtraBlock(pos, extra, flags);
    return true;
}

void History::push(ChangeSet changes) {
    if (changes.empty()) return;
    mRedo.clear();
    mUndo.push_back(std::move(changes));
    trim();
}

void History::pushUndo(ChangeSet changes) {
    if (changes.empty()) return;
    mUndo.push_back(std::move(changes));
    trim();
}

void History::pushRedo(ChangeSet changes) {
    if (changes.empty()) return;
    mRedo.push_back(std::move(changes));
    trim();
}

namespace {
std::vector<ChangeSet> popBack(std::deque<ChangeSet>& stack, size_t count) {
    std::vector<ChangeSet> result;
    while (count-- && !stack.empty()) {
        result.push_back(std::move(stack.back()));
        stack.pop_back();
    }
    return result;
}
} // namespace

std::vector<ChangeSet> History::popUndo(size_t count) { return popBack(mUndo, count); }

std::vector<ChangeSet> History::popRedo(size_t count) { return popBack(mRedo, count); }

void History::clear() {
    mUndo.clear();
    mRedo.clear();
}

void History::trim() {
    auto blocks = [this] {
        size_t total = 0;
        for (auto const& changes : mUndo) total += changes.size();
        for (auto const& changes : mRedo) total += changes.size();
        return total;
    };
    // 先丢最早的重做记录，再丢最早的撤销记录；最新的一条总是保留。
    while (mUndo.size() + mRedo.size() > 1
           && (mUndo.size() > config.history.size || blocks() > config.history.maxBlocksInMemory)) {
        if (mRedo.size() > 0 && (mUndo.size() <= config.history.size)) mRedo.pop_front();
        else mUndo.pop_front();
    }
}

} // namespace creatorx::edit
