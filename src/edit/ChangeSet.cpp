#include "edit/ChangeSet.h"

#include "mod/Global.h"
#include "storage/Nbt.h"
#include <chrono>
#include <atomic>
#include <optional>
#include <fmt/chrono.h>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/Actor.h"
#include "mc/legacy/ActorUniqueID.h"

namespace creatorx::edit {
namespace {

std::optional<core::Vec3i> secondPartOffset(Block const& block) {
    auto const& tag = *block.mSerializationId;
    if (!tag.contains("states")) return std::nullopt;
    auto const& states = tag["states"].get<CompoundTag>();
    if (states.contains("upper_block_bit")) return core::Vec3i{0, bool(states["upper_block_bit"]) ? -1 : 1, 0};
    if (states.contains("head_piece_bit") && states.contains("direction")) {
        static constexpr core::Vec3i direction[]{{0,0,1}, {-1,0,0}, {0,0,-1}, {1,0,0}};
        auto offset = direction[int(states["direction"]) & 3];
        return bool(states["head_piece_bit"]) ? core::Vec3i{} - offset : offset;
    }
    return std::nullopt;
}

std::optional<bool> hasSecondPart(BlockSource& region, core::Vec3i pos, Block const& block, core::Vec3i offset) {
    auto other = pos + offset;
    BlockPos otherPos{other.x, other.y, other.z};
    if (((pos.x >> 4) != (other.x >> 4) || (pos.z >> 4) != (other.z >> 4))
        && !region.getChunkSource().getAvailableChunk(ChunkPos{otherPos})) return std::nullopt;
    auto const& adjacent = region.getBlock(otherPos);
    return &adjacent.getBlockType() == &block.getBlockType()
        && secondPartOffset(adjacent) == core::Vec3i{} - offset;
}

int updateFlags(BlockSource& region, core::Vec3i pos, Block const& oldBlock, Block const& newBlock) {
    // 1 通知邻居，2 同步客户端。第一半拆除或就位时先只同步。
    if (!config.performance.physics) return 2;
    auto oldOffset = secondPartOffset(oldBlock);
    auto newOffset = secondPartOffset(newBlock);
    bool samePart = &oldBlock.getBlockType() == &newBlock.getBlockType() && oldOffset == newOffset;
    if (oldOffset && !samePart && hasSecondPart(region, pos, oldBlock, *oldOffset).value_or(true)) return 2;
    if (newOffset && !hasSecondPart(region, pos, newBlock, *newOffset).value_or(false)) return 2;
    return 3;
}

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

ChangeSet::ChangeSet(int dimension) : mDimension(dimension), mPool(std::make_shared<Pool>()) {
    created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

CompoundTag ChangeSet::serialize() const {
    if (!mPool) return storage::readNbt(mDiskPath);
    using storage::Nbt;
    CompoundTag tag;
    tag["dimension"] = mDimension;
    tag["label"] = label;
    tag["created"] = created;
    auto palette = Nbt::array();
    for (auto block : mPool->blocks) palette.push_back(*block->mSerializationId);
    tag["palette"] = std::move(palette);
    auto entities = Nbt::array();
    for (auto const& entity : mPool->entities) entities.push_back(*entity);
    tag["entities"] = std::move(entities);
    IntArrayTag entries;
    entries.reserve(mEntries.size()*9);
    for (auto const& e : mEntries) {
        entries.insert(entries.end(),{e.pos.x, e.pos.y, e.pos.z, int(e.oldBlock), int(e.oldExtra), int(e.newBlock), int(e.newExtra), e.oldEntity, e.newEntity});
    }
    tag["entries"] = std::move(entries);
    auto actors=Nbt::array();
    for(auto const& actor:mActors) actors.push_back(Nbt::object({
        {"pos",storage::vector(actor.pos)},{"id",actor.id},{"added",actor.added},{"nbt",*actor.tag}
    }));
    tag["actors"]=std::move(actors);
    return tag;
}

ChangeSet ChangeSet::deserialize(CompoundTag const& tag) {
    ChangeSet result{int(tag["dimension"])};
    result.label = std::string(std::string_view(tag["label"]));
    result.created = tag["created"].get<std::int64_t>();
    for (auto const& item : tag["palette"].get<ListTag>()) {
        auto block = Block::tryGetFromRegistry(item.get<CompoundTag>());
        if (!block) throw std::runtime_error("unknown block in history");
        result.mPool->indices.emplace(block.as_ptr(), std::uint32_t(result.mPool->blocks.size()));
        result.mPool->blocks.push_back(block.as_ptr());
    }
    for (auto const& entity : tag["entities"].get<ListTag>()) {
        result.mPool->entities.push_back(std::make_unique<CompoundTag>(entity.get<CompoundTag>()));
    }
    auto const& entries=tag["entries"].get<IntArrayTag>();
    if(entries.size()%9) throw std::runtime_error("invalid history entry");
    for(size_t i=0;i<entries.size();i+=9) {
        auto e=entries.data()+i;
        Entry entry{{e[0],e[1],e[2]}, std::uint32_t(e[3]),std::uint32_t(e[4]),std::uint32_t(e[5]),std::uint32_t(e[6]),e[7],e[8]};
        auto blocks = result.mPool->blocks.size();
        auto entities = result.mPool->entities.size();
        if (entry.oldBlock >= blocks || entry.oldExtra >= blocks || entry.newBlock >= blocks || entry.newExtra >= blocks
            || entry.oldEntity < -1 || entry.newEntity < -1
            || (entry.oldEntity >= 0 && size_t(entry.oldEntity) >= entities)
            || (entry.newEntity >= 0 && size_t(entry.newEntity) >= entities)) throw std::runtime_error("invalid history palette index");
        result.mEntries.push_back(entry);
    }
    if(tag.contains("actors")) for(auto const& value:tag["actors"].get<ListTag>()) {
        auto const& actor=value.get<CompoundTag>();
        result.mActors.push_back({storage::vector(actor["pos"]),actor["id"].get<std::int64_t>(),bool(actor["added"]),std::make_shared<CompoundTag>(actor["nbt"].get<CompoundTag>())});
    }
    return result;
}

bool ChangeSet::trySpill() {
    if (!mPool || empty()) return true;
    if (mDiskPath.empty()) persist();
    if (mWrite.valid()) {
        if (mWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        mWrite.get();
    }
    std::vector<Entry>{}.swap(mEntries);
    std::vector<ActorEntry>{}.swap(mActors);
    mPool.reset();
    return true;
}

void ChangeSet::persist() {
    if (empty()) return;
    if (!mDiskPath.empty()) {
        if (!mWrite.valid() || mWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            mWrite.get();
            return;
        } catch (std::exception const&) {
            // 上一次写入失败，保留的内存记录可以重新提交。
            mDiskPath.clear();
        }
    }
    static std::atomic_uint64_t sequence{0};
    auto nonce=std::chrono::system_clock::now().time_since_epoch().count();
    auto path = storage::dataDir() / "history-cache" / fmt::format("{}-{}.nbt", nonce, ++sequence);
    mWrite=storage::queueNbt(path, serialize());
    mStoredSize = size();
    mDiskPath = std::move(path);
}

CompoundTag ChangeSet::reference() {
    persist();
    CompoundTag tag;
    tag["file"]=mDiskPath.filename().string();
    tag["size"]=std::int64_t(size());
    tag["dimension"]=mDimension;
    tag["label"]=label;
    tag["created"]=created;
    return tag;
}

ChangeSet ChangeSet::fromReference(CompoundTag const& tag) {
    ChangeSet result{int(tag["dimension"])};
    std::string file{std::string_view(tag["file"])};
    if(std::filesystem::path(file).filename()!=file) throw std::runtime_error("invalid history file");
    result.mDiskPath=storage::dataDir()/"history-cache"/file;
    result.mStoredSize=tag["size"].get<size_t>();
    result.label=std::string(std::string_view(tag["label"]));
    result.created=tag["created"].get<std::int64_t>();
    result.mPool.reset();
    return result;
}

void ChangeSet::ensureLoaded() {
    if (mPool) return;
    if (mWrite.valid()) mWrite.get();
    auto path = mDiskPath;
    auto restored = deserialize(storage::readNbt(path));
    restored.mStoredSize = restored.size();
    restored.mDiskPath = std::move(path);
    *this = std::move(restored);
}

void ChangeSet::record(
    core::Vec3i                  pos,
    Block const&                 oldBlock,
    Block const&                 oldExtra,
    std::unique_ptr<CompoundTag> oldEntity,
    Block const&                 newBlock,
    Block const&                 newExtra,
    CompoundTag const*           newEntity
) {
    mDiskPath.clear();
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

void ChangeSet::recordActor(core::Vec3i pos,std::int64_t id,CompoundTag tag,bool added) {
    mDiskPath.clear();
    mActors.push_back({pos,id,added,std::make_shared<CompoundTag>(std::move(tag))});
}

void ChangeSet::apply(BlockSource& region, size_t index, bool undo) {
    if(index>=mEntries.size()) {
        auto& actor=mActors[index-mEntries.size()];
        mDiskPath.clear();
        if(actor.added==undo) {
            if(auto existing=region.getLevel().fetchEntity(ActorUniqueID{actor.id},false)) if(!existing->isPlayer()) existing->remove();
        } else {
            auto tag=*actor.tag;
            tag.erase("UniqueID");
            auto spawned=region.spawnActor(tag);
            if(!spawned) throw std::runtime_error("could not restore entity");
            actor.id=spawned->getOrCreateUniqueID().rawID;
        }
        return;
    }
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
    ensureLoaded();
    mDiskPath.clear();
    ChangeSet tail(mDimension);
    tail.mPool = mPool;
    tail.label = label;
    tail.created = created;
    if(from<mEntries.size()) {
        tail.mEntries.assign(mEntries.begin()+std::ptrdiff_t(from),mEntries.end());
        mEntries.resize(from);
        tail.mActors=std::move(mActors);
    } else {
        auto offset=from-mEntries.size();
        tail.mActors.assign(mActors.begin()+std::ptrdiff_t(offset),mActors.end());
        mActors.resize(offset);
    }
    return tail;
}

bool writeBlock(
    BlockSource&       region,
    core::Vec3i        p,
    Block const&       block,
    Block const&       extra,
    CompoundTag const* entity,
    ChangeSet*         changes,
    Block const*       knownOldBlock
) {
    BlockPos    pos{p.x, p.y, p.z};
    auto const& oldBlock = knownOldBlock ? *knownOldBlock : region.getBlock(pos);
    auto const& oldExtra = region.getExtraBlock(pos);
    if (&oldBlock == &block && &oldExtra == &extra && !entity) return false;

    std::optional<CompoundTag> relocated;
    if(entity) {
        relocated=*entity;
        if(entity->contains("pairx") && entity->contains("x")) (*relocated)["pairx"]=int((*entity)["pairx"])+p.x-int((*entity)["x"]);
        if(entity->contains("pairz") && entity->contains("z")) (*relocated)["pairz"]=int((*entity)["pairz"])+p.z-int((*entity)["z"]);
        (*relocated)["x"]=p.x; (*relocated)["y"]=p.y; (*relocated)["z"]=p.z;
        entity=&*relocated;
    }

    if (changes) {
        std::unique_ptr<CompoundTag> oldEntity;
        if (oldBlock.getBlockType().mBlockEntityType != BlockActorType::Undefined) {
            if (auto* actor = region.getBlockEntity(pos)) {
                oldEntity = std::make_unique<CompoundTag>();
                if (!actor->save(*oldEntity, SaveContext{})) oldEntity.reset();
            }
        }
        changes->record(p, oldBlock, oldExtra, std::move(oldEntity), block, extra, entity);
    }

    int                flags = updateFlags(region, p, oldBlock, block);
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

CompoundTag History::serialize() {
    CompoundTag tag;
    tag["version"] = 1;
    auto undo = storage::Nbt::array(), redo = storage::Nbt::array();
    for (auto& changes : mUndo) undo.push_back(changes.reference());
    for (auto& changes : mRedo) redo.push_back(changes.reference());
    // reference() 可能重新提交先前失败的写入，下一 tick 再检查内存预算。
    mSpillPending = true;
    tag["undo"] = std::move(undo);
    tag["redo"] = std::move(redo);
    return tag;
}

std::vector<std::shared_future<void>> History::pendingWrites() const {
    std::vector<std::shared_future<void>> result;
    for(auto stack:{&mUndo,&mRedo}) for(auto const& changes:*stack) if(auto write=changes.pendingWrite();write.valid()) result.push_back(std::move(write));
    return result;
}

void History::deserialize(CompoundTag const& tag) {
    if (int(tag["version"]) != 1) throw std::runtime_error("unsupported history version");
    std::deque<ChangeSet> undo, redo;
    for (auto const& changes : tag["undo"].get<ListTag>()) undo.push_back(ChangeSet::fromReference(changes.get<CompoundTag>()));
    for (auto const& changes : tag["redo"].get<ListTag>()) redo.push_back(ChangeSet::fromReference(changes.get<CompoundTag>()));
    mUndo = std::move(undo);
    mRedo = std::move(redo);
}

std::string History::describe() const {
    std::string result;
    auto describe=[&](auto const& stack,std::string_view heading) {
        result+=std::string(tr(heading))+"\n";
        size_t number=0;
        for(auto it=stack.rbegin();it!=stack.rend();++it) {
            auto timestamp=std::time_t(it->created);
            std::tm time{};
            localtime_s(&time,&timestamp);
            result+=fmt::format("{}. {} | {} | {:%Y-%m-%d %H:%M:%S}\n",++number,tr("operation."+it->label),it->size(),time);
        }
    };
    describe(mUndo,"history.undo_heading");
    describe(mRedo,"history.redo_heading");
    return result;
}

void History::trim() {
    while (mUndo.size() > config.history.size) mUndo.pop_front();
    mSpillPending = false;
    size_t total = 0;
    for (auto const& changes : mUndo) total += changes.residentSize();
    for (auto const& changes : mRedo) total += changes.residentSize();
    for (auto* stack : {&mRedo, &mUndo}) {
        for (auto& changes : *stack) {
            if (total <= config.history.maxBlocksInMemory) return;
            auto resident = changes.residentSize();
            try {
                if (!changes.trySpill()) mSpillPending = true;
                total -= resident;
            } catch (std::exception const&) {
                // 写入线程已记录错误；下次保存索引时重试。
            }
        }
    }
}

} // namespace creatorx::edit
