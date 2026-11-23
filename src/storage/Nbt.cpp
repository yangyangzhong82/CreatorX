#include "storage/Nbt.h"
#include "mod/Entry.h"
#include <Windows.h>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include "mod/Global.h"

namespace creatorx::storage {
namespace {
void writeBytes(std::filesystem::path const& path, std::string_view bytes);

struct Writer {
    struct Task {std::filesystem::path path; std::function<std::string()> source; std::promise<void> result; std::vector<std::shared_future<void>> dependencies; std::string errorTemplate;};
    struct Pending {
        size_t count = 0;
        std::shared_future<void> latest;
    };
    struct PathLess {
        bool operator()(std::filesystem::path const& a, std::filesystem::path const& b) const {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> queue;
    std::map<std::filesystem::path, Pending, PathLess> pending;
    bool stopping=false;
    std::thread worker;
    Writer() : worker([this] {
        std::unique_lock lock(mutex);
        for(;;) {
            changed.wait(lock,[&] {return stopping || !queue.empty();});
            if(queue.empty() && stopping) break;
            auto task=std::move(queue.front()); queue.pop_front();
            lock.unlock();
            try {
                for(auto const& dependency:task.dependencies) dependency.get();
                writeBytes(task.path, task.source());
                task.result.set_value();
            }
            catch(std::exception const& error) {task.result.set_exception(std::current_exception());logger.error(fmt::format(fmt::runtime(task.errorTemplate),error.what()));}
            lock.lock();
            auto entry = pending.find(task.path);
            if (--entry->second.count == 0) pending.erase(entry);
        }
    }) {}
    ~Writer() { {std::lock_guard lock(mutex);stopping=true;} changed.notify_one(); if(worker.joinable()) worker.join(); }
};
std::unique_ptr<Writer> writer;
}
void startStorage() {if(!writer) writer=std::make_unique<Writer>();}
std::shared_future<void> queueNbt(std::filesystem::path path, NbtSource source, std::vector<std::shared_future<void>> dependencies) {
    return queueBytes(std::move(path), [source = std::move(source)] {
        if (auto tag = std::get_if<CompoundTag>(&source)) return tag->toBinaryNbt();
        return std::get<std::function<CompoundTag()>>(source)().toBinaryNbt();
    }, std::move(dependencies));
}
std::shared_future<void> queueBytes(
    std::filesystem::path path, std::function<std::string()> source, std::vector<std::shared_future<void>> dependencies
) {
    startStorage();
    Writer::Task task{path.lexically_normal().make_preferred(),std::move(source),{},std::move(dependencies),std::string(tr("storage.failed"))};
    auto result=task.result.get_future().share();
    {
        std::lock_guard lock(writer->mutex);
        auto& pending = writer->pending[task.path];
        writer->queue.push_back(std::move(task));
        ++pending.count;
        pending.latest = result;
    }
    writer->changed.notify_one();
    return result;
}
std::shared_future<void> pendingWrite(std::filesystem::path const& path) {
    if (!writer) return {};
    std::lock_guard lock(writer->mutex);
    auto entry = writer->pending.find(path.lexically_normal().make_preferred());
    return entry != writer->pending.end() ? entry->second.latest : std::shared_future<void>{};
}
void waitForWrite(std::filesystem::path const& path) {
    auto latest = pendingWrite(path);
    if (latest.valid()) latest.wait();
}
void stopStorage() {writer.reset();}
std::filesystem::path dataDir() { return Entry::getInstance().getSelf().getDataDir(); }
CompoundTag readNbt(std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    auto tag = CompoundTag::fromBinaryNbt(bytes);
    if (!tag) throw std::runtime_error(tag.error().message());
    return std::move(*tag);
}
void writeNbt(std::filesystem::path const& path, CompoundTag const& tag) {
    writeBytes(path, tag.toBinaryNbt());
}
namespace {
void writeBytes(std::filesystem::path const& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), bytes.size());
    file.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot replace " + path.string());
    }
}
} // namespace
} // namespace creatorx::storage
