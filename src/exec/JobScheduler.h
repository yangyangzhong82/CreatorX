#pragma once

#include "mc/platform/UUID.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class BlockSource;
class Level;
class Player;

namespace creatorx::exec {

// 本 tick 分给一个任务的预算。
struct Budget {
    std::uint64_t                         units;
    std::chrono::steady_clock::time_point deadline;

    bool exhausted(std::uint64_t used) const {
        return used >= units || ((used & 255) == 0 && std::chrono::steady_clock::now() >= deadline);
    }
};

enum class JobState { running, done, failed };
enum class JobEnd { completed, cancelled, failed };

class Job {
public:
    virtual ~Job() = default;

    virtual std::uint64_t total() const    = 0;
    virtual std::uint64_t progress() const = 0;

    // 在预算内推进；返回 failed 前应把原因写入 mError。
    virtual JobState run(Level& level, Budget const& budget) = 0;

    // 任务结束（完成、取消或失败）后调用一次；玩家可能已离线，此时 player 为空。
    virtual void finish(Player* player, JobEnd end) = 0;

    // 完成后接着执行的任务，例如剪切 = 复制 + 清空。
    virtual std::unique_ptr<Job> next() { return nullptr; }

    std::string const& error() const { return mError; }

protected:
    std::string mError;
};

// 取维度的 BlockSource；维度不存在时返回空。
BlockSource* regionOf(Level& level, int dimension);

// 提交任务。每个玩家同时只能有一个任务；失败时 error 为原因。
bool canSubmit(mce::UUID const& uuid, std::string& error);
bool submitJob(Player& player, std::unique_ptr<Job> job, std::string& error);
bool hasJob(mce::UUID const& uuid);
// 玩家当前任务的进度百分比；没有任务时为空。
std::optional<unsigned> jobPercent(mce::UUID const& uuid);
bool cancelJob(mce::UUID const& uuid);

void enableScheduler();
void disableScheduler();

} // namespace creatorx::exec
