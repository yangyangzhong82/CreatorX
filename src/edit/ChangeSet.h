#pragma once

#include "core/math/Vec3i.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <string>
#include <future>

class Block;
class BlockSource;
class CompoundTag;

namespace creatorx::edit {

// 一次操作的撤销记录。方块按调色板下标存储；拆分后的两半共用同一个调色板。
class ChangeSet {
public:
    explicit ChangeSet(int dimension);

    int    dimension() const { return mDimension; }
    size_t size() const { return mDiskPath.empty() ? residentSize() : mStoredSize; }
    bool   empty() const { return size() == 0; }
    size_t residentSize() const { return mEntries.size()+mActors.size()+mBiomes.size(); }
    bool trySpill();
    void ensureLoaded();
    void persist();
    CompoundTag reference();
    static ChangeSet fromReference(CompoundTag const& tag);
    std::shared_future<void> pendingWrite() const {return mWrite;}
    CompoundTag serialize() const;
    static ChangeSet deserialize(CompoundTag const& tag);
    std::string label = "edit";
    std::int64_t created = 0;

    void record(
        core::Vec3i                  pos,
        Block const&                 oldBlock,
        Block const&                 oldExtra,
        std::unique_ptr<CompoundTag> oldEntity,
        Block const&                 newBlock,
        Block const&                 newExtra,
        CompoundTag const*           newEntity
    );
    void recordActor(core::Vec3i pos, std::int64_t id, CompoundTag tag, bool added);
    void recordBiome(core::Vec3i pos, std::string oldBiome, std::string newBiome);
    bool isBiome(size_t index) const { return index >= mEntries.size()+mActors.size(); }

    // 把第 index 条改动写回世界：undo 为 true 时写旧值，否则写新值。
    void apply(BlockSource& region, size_t index, bool undo);

    core::Vec3i position(size_t index) const {
        if (index < mEntries.size()) return mEntries[index].pos;
        index -= mEntries.size();
        return index < mActors.size() ? mActors[index].pos : mBiomes[index-mActors.size()].pos;
    }

    // 截下 [from, size) 作为新的 ChangeSet 返回，本对象保留 [0, from)。
    ChangeSet splitTail(size_t from);

private:
    struct Entry {
        core::Vec3i   pos;
        std::uint32_t oldBlock, oldExtra, newBlock, newExtra;
        std::int32_t  oldEntity, newEntity;
    };
    struct Pool {
        std::vector<Block const*>                       blocks;
        std::unordered_map<Block const*, std::uint32_t> indices;
        std::vector<std::unique_ptr<CompoundTag>>       entities;

        std::uint32_t indexOf(Block const& block);
        std::int32_t  addEntity(std::unique_ptr<CompoundTag> tag);
    };

    int                   mDimension;
    std::shared_ptr<Pool> mPool;
    std::vector<Entry>    mEntries;
    struct ActorEntry {
        core::Vec3i pos;
        std::int64_t id;
        bool added;
        std::shared_ptr<CompoundTag const> tag;
    };
    std::vector<ActorEntry> mActors;
    struct BiomeEntry {
        core::Vec3i pos;
        std::string oldBiome, newBiome;
    };
    std::vector<BiomeEntry> mBiomes;
    std::filesystem::path mDiskPath;
    size_t mStoredSize = 0;
    std::shared_future<void> mWrite;
};

// 写入一个方块（含第二层与方块实体）。changes 非空时记录旧值。返回是否真的改动了世界。
bool writeBlock(
    BlockSource&       region,
    core::Vec3i        pos,
    Block const&       block,
    Block const&       extra,
    CompoundTag const* entity,
    ChangeSet*         changes,
    Block const*       knownOldBlock = nullptr
);

// 每个玩家的撤销 / 重做栈。
class History {
public:
    // 新操作：清空重做栈。
    void push(ChangeSet changes);
    // 撤销 / 重做任务结束后把记录放回对应的栈，不清空另一侧。
    void pushUndo(ChangeSet changes);
    void pushRedo(ChangeSet changes);

    std::vector<ChangeSet> popUndo(size_t count);
    std::vector<ChangeSet> popRedo(size_t count);

    size_t undoCount() const { return mUndo.size(); }
    size_t redoCount() const { return mRedo.size(); }
    void   clear();
    CompoundTag serialize();
    void deserialize(CompoundTag const& tag);
    void pollSpills() { if (mSpillPending) trim(); }
    std::string describe() const;
    std::vector<std::shared_future<void>> pendingWrites() const;

private:
    void trim();

    std::deque<ChangeSet> mUndo;
    std::deque<ChangeSet> mRedo;
    bool mSpillPending = false;
};

} // namespace creatorx::edit
