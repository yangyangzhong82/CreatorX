#include "exec/JobScheduler.h"

#include "mod/Global.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace creatorx::exec {
namespace {

struct Running {
    mce::UUID                             owner;
    std::unique_ptr<Job>                  job;
    std::chrono::steady_clock::time_point started;
};

std::vector<Running>   jobs;
ll::event::ListenerPtr tickListener;
int                    progressTicks = 0;

Player* playerOf(mce::UUID const& uuid) {
    auto level = ll::service::getLevel();
    return level ? level->getPlayer(uuid) : nullptr;
}

std::string formatCount(std::uint64_t value) {
    if (value >= 1000000) return fmt::format("{:.1f}M", double(value) / 1e6);
    if (value >= 10000) return fmt::format("{:.1f}K", double(value) / 1e3);
    return std::to_string(value);
}

void showProgress(Running const& running) {
    auto* player = playerOf(running.owner);
    if (!player) return;
    auto total   = std::max<std::uint64_t>(running.job->total(), 1);
    auto done    = std::min(running.job->progress(), total);
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - running.started).count();
    auto eta     = done ? elapsed * double(total - done) / double(done) : 0.0;
    auto text    = tr("job.progress", done * 100 / total, formatCount(done), formatCount(total), int(std::ceil(eta)));
    SetTitlePacket packet(SetTitlePacketPayload::TitleType::Actionbar, text, std::nullopt);
    packet.sendTo(*player);
}

void tick(Level& level) {
    if (jobs.empty()) return;
    auto const share = jobs.size();
    auto const units = std::max<std::uint64_t>(config.performance.blocksPerTick / share, 1);
    auto const time  = std::chrono::microseconds(
        std::max<std::int64_t>(std::int64_t(config.performance.tickBudgetMs * 1000.0 / double(share)), 100)
    );
    for (size_t i = 0; i < jobs.size();) {
        auto state = jobs[i].job->run(level, Budget{units, std::chrono::steady_clock::now() + time});
        if (state == JobState::running) {
            ++i;
            continue;
        }
        auto finished = std::move(jobs[i]);
        jobs.erase(jobs.begin() + std::ptrdiff_t(i));
        finished.job->finish(playerOf(finished.owner), state == JobState::done ? JobEnd::completed : JobEnd::failed);
        if (state != JobState::done) continue;
        if (auto next = finished.job->next()) {
            jobs.insert(jobs.begin() + std::ptrdiff_t(i), Running{finished.owner, std::move(next), finished.started});
            ++i;
        }
    }
    if (++progressTicks >= 20) {
        progressTicks = 0;
        for (auto const& running : jobs) showProgress(running);
    }
}

} // namespace

BlockSource* regionOf(Level& level, int dimension) {
    auto target = level.getDimension(DimensionType(dimension)).lock();
    return target ? &target->getBlockSourceFromMainChunkSource() : nullptr;
}

bool canSubmit(mce::UUID const& uuid, std::string& error) {
    if (hasJob(uuid)) {
        error = tr("job.busy");
        return false;
    }
    if (jobs.size() >= std::max(config.performance.maxConcurrentJobs, 1u)) {
        error = tr("job.server_busy");
        return false;
    }
    return true;
}

bool submitJob(Player& player, std::unique_ptr<Job> job, std::string& error) {
    auto uuid = player.getUuid();
    if (!canSubmit(uuid, error)) return false;
    jobs.push_back({uuid, std::move(job), std::chrono::steady_clock::now()});
    return true;
}

bool hasJob(mce::UUID const& uuid) {
    return std::ranges::any_of(jobs, [&](Running const& running) { return running.owner == uuid; });
}

std::optional<unsigned> jobPercent(mce::UUID const& uuid) {
    auto it = std::ranges::find_if(jobs, [&](Running const& running) { return running.owner == uuid; });
    if (it == jobs.end()) return std::nullopt;
    auto total = std::max<std::uint64_t>(it->job->total(), 1);
    return unsigned(std::min(it->job->progress(), total) * 100 / total);
}

bool cancelJob(mce::UUID const& uuid) {
    auto it = std::ranges::find_if(jobs, [&](Running const& running) { return running.owner == uuid; });
    if (it == jobs.end()) return false;
    auto cancelled = std::move(*it);
    jobs.erase(it);
    cancelled.job->finish(playerOf(cancelled.owner), JobEnd::cancelled);
    return true;
}

void enableScheduler() {
    tickListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>(
        [](ll::event::ServerLevelTickEvent& event) { tick(event.level()); }
    );
}

void disableScheduler() {
    if (tickListener) ll::event::EventBus::getInstance().removeListener(tickListener);
    tickListener.reset();
    while (!jobs.empty()) cancelJob(jobs.front().owner);
}

} // namespace creatorx::exec
