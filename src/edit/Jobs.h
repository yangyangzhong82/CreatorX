#pragma once

#include "core/region/Box.h"
#include "edit/ChangeSet.h"
#include "edit/Clipboard.h"
#include "edit/Pattern.h"
#include "exec/JobScheduler.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace creatorx::edit {

// set / replace / walls / faces：遍历包围盒，用图案写入通过掩码的方块。
std::unique_ptr<exec::Job> makeRegionEdit(
    mce::UUID           owner,
    int                 dimension,
    core::Box           box,
    core::BoxShape      shape,
    Pattern             pattern,
    std::optional<Mask> mask,
    std::vector<CompoundTag> removedActors = {}
);

// 复制选区到剪贴板；cutPattern 有值时复制完成后再用它填充选区（剪切）。
using CopyDestination = std::function<std::unique_ptr<exec::Job>(std::shared_ptr<Clipboard const>)>;
std::unique_ptr<exec::Job> makeCopy(
    mce::UUID              owner,
    int                    dimension,
    core::Box              box,
    core::Vec3i            offset,
    std::optional<Pattern> cutPattern,
    std::optional<Mask> mask = {},
    bool includeEntities = false,
    CopyDestination destination = {}
);

std::unique_ptr<exec::Job>
makePaste(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir, std::optional<Mask> mask = {}, bool includeEntities = false, bool maskTarget = false);

// iterations 为 0 时执行重力，其余情况用指定次数平滑地表。
std::unique_ptr<exec::Job> makeTerrainBrush(
    mce::UUID owner, int dimension, core::Vec3i center, core::Box box, int radius, int iterations,
    std::optional<Mask> mask
);

// 撤销或重做 sets（按弹出顺序处理）；中途取消时未写回的部分退回原来的栈。
std::unique_ptr<exec::Job> makeHistory(mce::UUID owner, bool undo, std::vector<ChangeSet> sets, mce::UUID requester);

using EditFilter = std::function<bool(BlockSource&, core::Vec3i)>;
std::unique_ptr<exec::Job> makeComputedEdit(mce::UUID owner, int dimension, core::Box box, Pattern pattern, EditFilter filter, std::string label, bool neighbors = false);
std::unique_ptr<exec::Job> makeInspect(mce::UUID owner, int dimension, core::Box box, std::optional<Mask> mask);
std::unique_ptr<exec::Job> makeClipboardInspect(mce::UUID owner,std::shared_ptr<Clipboard const> clipboard);
std::unique_ptr<exec::Job> makeTransfer(
    mce::UUID owner, int dimension, core::Box source, core::Vec3i offset, int copies,
    bool move, bool skipAir, std::optional<Mask> mask, bool includeEntities
);

} // namespace creatorx::edit
