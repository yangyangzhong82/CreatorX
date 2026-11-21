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

namespace creatorx::edit {

// set / replace / walls / faces：遍历包围盒，用图案写入通过掩码的方块。
std::unique_ptr<exec::Job> makeRegionEdit(
    mce::UUID           owner,
    int                 dimension,
    core::Box           box,
    core::BoxShape      shape,
    Pattern             pattern,
    std::optional<Mask> mask
);

// 复制选区到剪贴板；cutPattern 有值时复制完成后再用它填充选区（剪切）。
std::unique_ptr<exec::Job> makeCopy(
    mce::UUID              owner,
    int                    dimension,
    core::Box              box,
    core::Vec3i            offset,
    std::optional<Pattern> cutPattern
);

std::unique_ptr<exec::Job>
makePaste(mce::UUID owner, int dimension, std::shared_ptr<Clipboard const> clipboard, core::Vec3i min, bool skipAir);

// 撤销或重做 sets（按弹出顺序处理）；中途取消时未写回的部分退回原来的栈。
std::unique_ptr<exec::Job> makeHistory(mce::UUID owner, bool undo, std::vector<ChangeSet> sets);

} // namespace creatorx::edit
