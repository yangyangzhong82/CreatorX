#include "storage/Nbt.h"
#include "mod/Entry.h"
#include <Windows.h>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "mod/Global.h"

namespace creatorx::storage {
namespace {
struct Writer {
    struct Task {std::filesystem::path path; NbtSource source; std::promise<void> result; std::vector<std::shared_future<void>> dependencies; std::string errorTemplate;};
    std::mutex mutex;
    std::condition_variable changed, idle;
    std::deque<Task> queue;
    bool stopping=false, busy=false;
    std::thread worker;
    Writer() : worker([this] {
        std::unique_lock lock(mutex);
        for(;;) {
            changed.wait(lock,[&] {return stopping || !queue.empty();});
            if(queue.empty() && stopping) break;
            auto task=std::move(queue.front()); queue.pop_front(); busy=true;
            lock.unlock();
            try {
                for(auto const& dependency:task.dependencies) dependency.get();
                if (auto tag = std::get_if<CompoundTag>(&task.source)) writeNbt(task.path, *tag);
                else writeNbt(task.path, std::get<std::function<CompoundTag()>>(task.source)());
                task.result.set_value();
            }
            catch(std::exception const& error) {task.result.set_exception(std::current_exception());logger.error(fmt::format(fmt::runtime(task.errorTemplate),error.what()));}
            lock.lock(); busy=false;
            if(queue.empty()) idle.notify_all();
        }
    }) {}
    ~Writer() { {std::lock_guard lock(mutex);stopping=true;} changed.notify_one(); if(worker.joinable()) worker.join(); }
};
std::unique_ptr<Writer> writer;
}
void startStorage() {if(!writer) writer=std::make_unique<Writer>();}
std::shared_future<void> queueNbt(std::filesystem::path path, NbtSource source, std::vector<std::shared_future<void>> dependencies) {
    startStorage();
    Writer::Task task{std::move(path),std::move(source),{},std::move(dependencies),std::string(tr("storage.failed"))};
    auto result=task.result.get_future().share();
    {std::lock_guard lock(writer->mutex);writer->queue.push_back(std::move(task));}
    writer->changed.notify_one();
    return result;
}
void flushWrites() {
    if(!writer) return;
    std::unique_lock lock(writer->mutex);
    writer->idle.wait(lock,[] {return writer->queue.empty() && !writer->busy;});
}
void stopStorage() {writer.reset();}
std::filesystem::path dataDir() { return Entry::getInstance().getSelf().getDataDir(); }
CompoundTag readNbt(std::filesystem::path const& path) {
    flushWrites();
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    auto tag = CompoundTag::fromBinaryNbt(bytes);
    if (!tag) throw std::runtime_error(tag.error().message());
    return std::move(*tag);
}
void writeNbt(std::filesystem::path const& path, CompoundTag const& tag) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    auto bytes = tag.toBinaryNbt();
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), bytes.size());
    file.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot replace " + path.string());
    }
}
} // namespace creatorx::storage
