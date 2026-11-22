#include "command/AdvancedCommands.h"
#include "command/EditCommands.h"
#include "command/SelectionCommands.h"
#include "edit/Jobs.h"
#include "schematic/Schematic.h"
#include "session/PlayerSession.h"
#include "storage/Nbt.h"
#include "mod/Global.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandRawText.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include <charconv>
#include <cmath>
#include <map>
#include <future>

namespace creatorx {
struct AdvancedArgs { ll::command::Optional<CommandRawText> arguments; };
namespace {
ll::event::ListenerPtr listener;
constexpr std::string_view names[]{"count", "distr", "hollow", "overlay", "center", "sphere", "hsphere", "cyl", "hcyl", "pyramid", "hpyramid", "schem", "move", "stack"};
int integer(std::string_view value, int min = 1, int max = 512) {
    int result;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || end != value.data() + value.size() || result < min || result > max) throw std::runtime_error(tr("advanced.bad_number", min, max));
    return result;
}
class SchematicJob final : public exec::Job {
public:
    SchematicJob(mce::UUID owner,std::future<CompoundTag> read) : mOwner(owner),mRead(std::move(read)) {}
    SchematicJob(mce::UUID owner,std::shared_future<void> write) : mOwner(owner),mWrite(std::move(write)) {}
    std::uint64_t total() const override {return 1;}
    std::uint64_t progress() const override {return 0;}
    exec::JobState run(Level&,exec::Budget const&) override {
        try {
            if(mRead.valid()) {
                if(mRead.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return exec::JobState::running;
                auto clipboard=schematic::deserialize(mRead.get());
                if(auto session=findSession(mOwner)) session->clipboard=std::move(clipboard);
                saveClipboard(mOwner);
            } else {
                if(mWrite.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return exec::JobState::running;
                mWrite.get();
            }
            return exec::JobState::done;
        } catch(std::exception const& error) {mError=tr("advanced.failed",error.what());return exec::JobState::failed;}
    }
    void finish(Player* player,exec::JobEnd end) override {
        if(player) player->sendMessage(end==exec::JobEnd::completed ? tr("schematic.done") : end==exec::JobEnd::cancelled ? (mWrite.valid() ? tr("schematic.save_continues") : tr("inspect.cancelled")) : std::string_view(mError));
    }
private:
    mce::UUID mOwner;
    std::future<CompoundTag> mRead;
    std::shared_future<void> mWrite;
};
void execute(Player& player, Feedback const& output, std::string_view name, std::string_view raw) {
    auto tokens = edit::splitTokens(raw);
    if (name == "move" || name == "stack") {
        auto selection = completeSelection(player, output);
        if (!selection) return;
        int amount = 1;
        size_t cursor = 0;
        if (!tokens.empty() && !tokens[0].starts_with('-')
            && tokens[0].find_first_not_of("0123456789") == std::string_view::npos) { amount = integer(tokens[0],1,4096); cursor = 1; }
        auto view = player.getViewVector();
        core::Vec3i dir{};
        if (std::abs(view.y) >= std::max(std::abs(view.x),std::abs(view.z))) dir.y = view.y > 0 ? 1 : -1;
        else if (std::abs(view.x) >= std::abs(view.z)) dir.x = view.x > 0 ? 1 : -1;
        else dir.z = view.z > 0 ? 1 : -1;
        bool skipAir = false, select = false;
        for (;cursor<tokens.size();++cursor) {
            auto token = tokens[cursor];
            if (token == "north") dir={0,0,-1}; else if (token=="south") dir={0,0,1};
            else if (token=="east") dir={1,0,0}; else if(token=="west") dir={-1,0,0};
            else if(token=="up") dir={0,1,0}; else if(token=="down") dir={0,-1,0};
            else if(token=="-a") skipAir=true; else if(token=="-s") select=true;
            else throw std::runtime_error(std::string(tr("advanced.transfer_usage")));
        }
        auto source=*selection->box();
        auto size=source.size();
        bool move=name=="move";
        core::Vec3i offset = move ? core::Vec3i{dir.x*amount,dir.y*amount,dir.z*amount} : core::Vec3i{dir.x*size.x,dir.y*size.y,dir.z*size.z};
        int copies=move ? 1 : amount;
        auto delta=core::Vec3i{offset.x*copies,offset.y*copies,offset.z*copies};
        auto destination=source.shifted(delta);
        auto const& height=*player.getDimension().mHeightRange;
        if(destination.min.y<height.mMin || destination.max.y>=height.mMax) throw std::runtime_error(std::string(tr("clipboard.out_of_height")));
        auto changed=source.volume()*(copies+(move ? 1 : 0));
        auto job=edit::makeTransfer(player.getUuid(),player.getDimensionId(),source,offset,copies,move,skipAir);
        std::function<void(Player&)> follow;
        if(select) follow=[destination](Player& target) {selectBox(target,destination);};
        submitEditJob(player,output,std::move(job),changed,std::move(follow));
        return;
    }
    if (name == "schem") {
        if (tokens.empty()) throw std::runtime_error(std::string(tr("schematic.usage")));
        if (tokens[0] == "list") {
            output.success(schematic::list(tokens.size() > 1 ? tokens[1] : std::string_view{}));
            return;
        }
        if (tokens.size() < 2) throw std::runtime_error(std::string(tr("schematic.usage")));
        auto path = schematic::path(std::string(tokens[1]), tokens.size() > 2 ? std::string(tokens[2]) : "cxs");
        std::string error;
        if(!exec::canSubmit(player.getUuid(),error)) {output.error(error);return;}
        std::unique_ptr<exec::Job> job;
        if (tokens[0] == "save") {
            auto clipboard = getSession(player).clipboard;
            if (!clipboard) throw std::runtime_error(std::string(tr("clipboard.empty")));
            auto write = storage::queueNbt(path, [clipboard, native = path.extension() == ".mcstructure"] {
                return schematic::serialize(*clipboard, native);
            });
            job = std::make_unique<SchematicJob>(player.getUuid(), std::move(write));
        } else if (tokens[0] == "load") {
            getSession(player);
            if (std::filesystem::file_size(path) > 256ull * 1024 * 1024) throw std::runtime_error(std::string(tr("schematic.too_large")));
            storage::flushWrites();
            job=std::make_unique<SchematicJob>(player.getUuid(),std::async(std::launch::async,[path] {return storage::readNbt(path);}));
        } else if (tokens[0] == "delete") {
            storage::flushWrites();
            if (!std::filesystem::remove(path)) throw std::runtime_error(std::string(tr("schematic.missing")));
        } else throw std::runtime_error(std::string(tr("schematic.usage")));
        if(job) {
            if(!exec::submitJob(player,std::move(job),error)) output.error(error);
            return;
        }
        output.success(tr("schematic.done"));
        return;
    }
    if (name == "distr" && raw == "-c") {
        auto clipboard = getSession(player).clipboard;
        if (!clipboard) throw std::runtime_error(std::string(tr("clipboard.empty")));
        std::string error;
        if(!exec::submitJob(player,edit::makeClipboardInspect(player.getUuid(),std::move(clipboard)),error)) output.error(error);
        return;
    }
    if (name == "count" || name == "distr") {
        auto selection = completeSelection(player, output);
        if (!selection) return;
        std::optional<edit::Mask> mask;
        if (name == "count") {
            std::string error;
            mask = edit::Mask::parse(raw, player, error);
            if (!mask) throw std::runtime_error(error);
        }
        auto job = edit::makeInspect(player.getUuid(), player.getDimensionId(), *selection->box(), std::move(mask));
        std::string error;
        if (!exec::submitJob(player, std::move(job), error)) output.error(error);
        return;
    }
    auto feet = player.getFeetBlockPos();
    core::Box box;
    edit::EditFilter filter;
    std::string_view patternText;
    if (name == "hollow" || name == "overlay" || name == "center") {
        auto selection = completeSelection(player, output);
        if (!selection) return;
        box = *selection->box();
        if (name == "hollow") {
            bool hasThickness=!tokens.empty() && tokens[0].find_first_not_of("0123456789")==std::string_view::npos;
            int thickness=hasThickness ? integer(tokens[0]) : 1;
            patternText=hasThickness ? (tokens.size()>1 ? raw.substr(tokens[1].data()-raw.data()) : std::string_view("air")) : (tokens.empty() ? std::string_view("air") : raw);
            filter = [box, thickness](BlockSource&, core::Vec3i p) {
                return p.x >= box.min.x + thickness && p.x <= box.max.x - thickness
                    && p.y >= box.min.y + thickness && p.y <= box.max.y - thickness
                    && p.z >= box.min.z + thickness && p.z <= box.max.z - thickness;
            };
        } else if (name == "center") {
            patternText = raw;
            auto low = core::Vec3i{int((std::int64_t(box.min.x) + box.max.x) / 2), int((std::int64_t(box.min.y) + box.max.y) / 2), int((std::int64_t(box.min.z) + box.max.z) / 2)};
            box = {low, low};
            filter = [](BlockSource&, core::Vec3i) { return true; };
        } else {
            patternText = raw;
            auto original = box;
            ++box.max.y;
            auto heights = std::make_shared<std::map<std::pair<int,int>, int>>();
            filter = [original, heights](BlockSource& region, core::Vec3i p) {
                auto key = std::pair{p.x, p.z};
                auto [it, inserted] = heights->try_emplace(key, original.min.y - 2);
                if (inserted) for (int y = original.max.y; y >= original.min.y; --y) {
                    if (region.getBlock({p.x, y, p.z})._isSolid()) { it->second = y; break; }
                }
                return p.y == it->second + 1;
            };
        }
    } else {
        size_t argument=1;
        while(argument<tokens.size() && tokens[argument-1].ends_with(',')) ++argument;
        if (tokens.size() <= argument) throw std::runtime_error(std::string(tr("advanced.shape_usage")));
        patternText=raw.substr(0,tokens[argument].data()-raw.data());
        bool hollow = name.starts_with('h');
        if (name == "sphere" || name == "hsphere" || name == "cyl" || name == "hcyl") {
            std::vector<int> radii;
            auto values = tokens[argument];
            for (;;) {
                auto comma = values.find(',');
                radii.push_back(integer(values.substr(0, comma)));
                if (comma == std::string_view::npos) break;
                values.remove_prefix(comma + 1);
            }
            bool sphere = name == "sphere" || name == "hsphere";
            if (radii.size() > (sphere ? 3u : 2u)) throw std::runtime_error(std::string(tr("advanced.shape_usage")));
            int rx = radii[0], ry = sphere && radii.size() > 1 ? radii[1] : radii[0];
            int rz = radii.size() > (sphere ? 2u : 1u) ? radii.back() : radii[0];
            int height = !sphere && tokens.size() > argument+1 ? integer(tokens[argument+1], 1, 2048) : 1;
            int cy = feet.y;
            if (sphere && tokens.size() > argument+1) {
                if(tokens[argument+1]!="-r") throw std::runtime_error(std::string(tr("advanced.shape_usage")));
                cy+=ry;
            }
            if(tokens.size()>argument+2) throw std::runtime_error(std::string(tr("advanced.shape_usage")));
            box = {{feet.x-rx, sphere ? cy-ry : cy, feet.z-rz}, {feet.x+rx, sphere ? cy+ry : cy+height-1, feet.z+rz}};
            auto inside = [=](core::Vec3i p) {
                double x = double(p.x-feet.x)/(rx+0.5), z = double(p.z-feet.z)/(rz+0.5);
                double y = sphere ? double(p.y-cy)/(ry+0.5) : 0;
                return x*x+y*y+z*z <= 1;
            };
            filter = [=](BlockSource&, core::Vec3i p) {
                if (!inside(p)) return false;
                if (!hollow) return true;
                return !inside(p+core::Vec3i{1,0,0}) || !inside(p+core::Vec3i{-1,0,0})
                    || !inside(p+core::Vec3i{0,0,1}) || !inside(p+core::Vec3i{0,0,-1})
                    || (sphere && (!inside(p+core::Vec3i{0,1,0}) || !inside(p+core::Vec3i{0,-1,0})));
            };
        } else {
            int layers = integer(tokens[argument]);
            if(tokens.size()>argument+1) throw std::runtime_error(std::string(tr("advanced.shape_usage")));
            box = {{feet.x-layers+1,feet.y,feet.z-layers+1},{feet.x+layers-1,feet.y+layers-1,feet.z+layers-1}};
            filter = [=](BlockSource&, core::Vec3i p) {
                int extent = layers - 1 - (p.y-feet.y);
                int distance = std::max(std::abs(p.x-feet.x), std::abs(p.z-feet.z));
                return hollow ? distance == extent : distance <= extent;
            };
        }
    }
    auto const& range = *player.getDimension().mHeightRange;
    if (box.min.y < range.mMin || box.max.y >= range.mMax) throw std::runtime_error(std::string(tr("clipboard.out_of_height")));
    if (box.volume() > config.limits.maxSelectionVolume) throw std::runtime_error(tr("selection.too_large", config.limits.maxSelectionVolume));
    std::string error;
    auto pattern = edit::Pattern::parse(patternText, player, error);
    if (!pattern) throw std::runtime_error(error);
    auto job = edit::makeComputedEdit(player.getUuid(), player.getDimensionId(), box, std::move(*pattern), std::move(filter), std::string(name));
    submitEditJob(player, output, std::move(job), box.volume());
}
void registerCommands() {
    auto& registrar = ll::command::CommandRegistrar::getServerInstance();
    auto& root = registrar.getOrCreateCommand("cx", std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors);
    for (auto name : names) {
        auto callback = [name](CommandOrigin const& origin, CommandOutput& output, AdvancedArgs const& args) {
            auto player = getCommandPlayer(origin, output);
            if (!player) return;
            try { execute(*player, output, name, args.arguments ? std::string_view(args.arguments->mText) : std::string_view{}); }
            catch (std::exception const& error) { output.error(tr("advanced.failed", error.what())); }
        };
        root.overload<AdvancedArgs>().text(name).optional("arguments").execute(callback);
        if (config.command.shortCommands) {
            registrar.getOrCreateCommand(std::string(name), std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors)
                .overload<AdvancedArgs>().optional("arguments").execute(callback);
        }
    }
}
} // namespace
void executeAdvanced(Player& player,Feedback const& output,std::string_view operation,std::string_view arguments) {
    try {execute(player,output,operation,arguments);}
    catch(std::exception const& error) {output.error(tr("advanced.failed",error.what()));}
}
void enableAdvancedCommands() {
    listener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>([](auto&) { registerCommands(); }, ll::event::EventPriority::Low);
    if (ll::service::getCommandRegistry(false)) registerCommands();
}
void disableAdvancedCommands() {
    if (listener) ll::event::EventBus::getInstance().removeListener(listener);
    listener.reset();
}
} // namespace creatorx
