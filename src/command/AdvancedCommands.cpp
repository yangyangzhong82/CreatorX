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
#include <limits>
#include <utility>

namespace creatorx {
struct AdvancedArgs { ll::command::Optional<CommandRawText> arguments; };

enum class AdvancedAction {
    count, distr, hollow, overlay, center, sphere, hsphere, cyl, hcyl, pyramid, hpyramid, schem, move, stack
};

struct AdvancedSubcommand {
    AdvancedAction action;
    ll::command::Optional<CommandRawText> arguments;
};

namespace {
ll::event::ListenerPtr listener;
int integer(std::string_view value, int min = 1, int max = 512) {
    int result;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || end != value.data() + value.size() || result < min || result > max) throw std::runtime_error(tr("advanced.bad_number", min, max));
    return result;
}

core::Vec3i selectionCenter(core::Box const& box) {
    auto middle = [](int min, int max) { return int((std::int64_t(min) + max) / 2); };
    return {middle(box.min.x, box.max.x), middle(box.min.y, box.max.y), middle(box.min.z, box.max.z)};
}

core::Box shapeBounds(core::Vec3i center, int rx, int rz, int bottom, int top) {
    auto offset = [](int coordinate, int delta) {
        auto value = std::int64_t(coordinate) + delta;
        if (!std::in_range<int>(value) || value == std::numeric_limits<int>::max()) {
            throw std::runtime_error(std::string(tr("advanced.shape_coordinates")));
        }
        return int(value);
    };
    return {
        {offset(center.x, -rx), offset(center.y, bottom), offset(center.z, -rz)},
        {offset(center.x, rx), offset(center.y, top), offset(center.z, rz)}
    };
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

bool runSchematic(Player& player, Feedback const& output, std::string_view action, std::string name, std::string format) {
    auto path = schematic::path(std::move(name), std::move(format));
    std::string error;
    if (!exec::canSubmit(player.getUuid(), error)) {
        output.error(error);
        return false;
    }

    std::unique_ptr<exec::Job> job;
    if (action == "save") {
        auto clipboard = getSession(player).clipboard;
        if (!clipboard) throw std::runtime_error(std::string(tr("clipboard.empty")));
        auto write = storage::queueNbt(path, [clipboard, native = path.extension() == ".mcstructure"] {
            return schematic::serialize(*clipboard, native);
        });
        job = std::make_unique<SchematicJob>(player.getUuid(), std::move(write));
    } else if (action == "load") {
        getSession(player);
        storage::waitForWrite(path);
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error(std::string(tr("schematic.missing")));
        if (std::filesystem::file_size(path) > 256ull * 1024 * 1024) throw std::runtime_error(std::string(tr("schematic.too_large")));
        job = std::make_unique<SchematicJob>(
            player.getUuid(), std::async(std::launch::async, [path] { return storage::readNbt(path); })
        );
    } else if (action == "delete") {
        storage::waitForWrite(path);
        if (!std::filesystem::remove(path)) throw std::runtime_error(std::string(tr("schematic.missing")));
        output.success(tr("schematic.done"));
        return true;
    } else {
        throw std::runtime_error(std::string(tr("schematic.usage")));
    }

    if (exec::submitJob(player, std::move(job), error)) return true;
    output.error(error);
    return false;
}

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
        bool skipAir = false, select = false, includeEntities = false;
        std::optional<edit::Mask> mask;
        for (;cursor<tokens.size();++cursor) {
            auto token = tokens[cursor];
            if (token == "north") dir={0,0,-1}; else if (token=="south") dir={0,0,1};
            else if (token=="east") dir={1,0,0}; else if(token=="west") dir={-1,0,0};
            else if(token=="up") dir={0,1,0}; else if(token=="down") dir={0,-1,0};
            else if(token=="-a") skipAir=true; else if(token=="-s") select=true;
            else if (token == "-e") includeEntities = true;
            else if (token == "-m") {
                if (++cursor == tokens.size()) throw std::runtime_error(std::string(tr("mask.empty")));
                std::string error;
                mask = edit::Mask::parse(raw.substr(tokens[cursor].data() - raw.data()), player, error);
                if (!mask) throw std::runtime_error(error);
                break;
            }
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
        auto job = edit::makeTransfer(
            player.getUuid(), player.getDimensionId(), source, offset, copies, move, skipAir, std::move(mask), includeEntities
        );
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
        runSchematic(player, output, tokens[0], std::string(tokens[1]), tokens.size() > 2 ? std::string(tokens[2]) : "cxs");
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
        if (!exec::submitJob(player, std::move(job), error)) {
            output.error(error);
        } else if (name == "count" && raw != "#existing" && raw != "hand" && raw != "offhand") {
            rememberRecent(getSession(player).recentMasks, raw);
        }
        return;
    }
    core::Box box;
    edit::EditFilter filter;
    std::string_view patternText;
    std::string_view maskText;
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
            auto center = selectionCenter(box);
            box = {center, center};
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
        auto extent = tokens[argument++];
        bool sphere = name == "sphere" || name == "hsphere";
        bool cylinder = name == "cyl" || name == "hcyl";
        bool hollow = name.starts_with('h');
        int height = 1;
        if (cylinder && argument < tokens.size() && !tokens[argument].starts_with('-')) {
            height = integer(tokens[argument++], 1, 2048);
        }
        bool raised = false, atSelection = false;
        for (; argument < tokens.size(); ++argument) {
            auto flag = tokens[argument];
            if (flag == "-c") atSelection = true;
            else if (flag == "-r" && sphere) raised = true;
            else if (flag == "-m") {
                if (++argument == tokens.size()) throw std::runtime_error(std::string(tr("mask.empty")));
                maskText = raw.substr(tokens[argument].data() - raw.data());
                break;
            } else throw std::runtime_error(std::string(tr("advanced.shape_usage")));
        }
        auto feet = player.getFeetBlockPos();
        core::Vec3i center{feet.x, feet.y, feet.z};
        if (atSelection) {
            auto* selection = completeSelection(player, output);
            if (!selection) return;
            center = selectionCenter(*selection->box());
        }
        if (sphere || cylinder) {
            std::vector<int> radii;
            auto values = extent;
            for (;;) {
                auto comma = values.find(',');
                radii.push_back(integer(values.substr(0, comma)));
                if (comma == std::string_view::npos) break;
                values.remove_prefix(comma + 1);
            }
            if (radii.size() > (sphere ? 3u : 2u)) throw std::runtime_error(std::string(tr("advanced.shape_usage")));
            int rx = radii[0], ry = sphere && radii.size() > 1 ? radii[1] : radii[0];
            int rz = radii.size() > (sphere ? 2u : 1u) ? radii.back() : radii[0];
            int cy = raised ? ry : 0;
            box = shapeBounds(center, rx, rz, sphere ? cy - ry : 0, sphere ? cy + ry : height - 1);
            auto inside = [=](core::Vec3i p) {
                double x = double(p.x)/(rx+0.5), z = double(p.z)/(rz+0.5);
                double y = sphere ? double(p.y-cy)/(ry+0.5) : 0;
                return x*x+y*y+z*z <= 1;
            };
            filter = [=](BlockSource&, core::Vec3i p) {
                p = p - center;
                if (!inside(p)) return false;
                if (!hollow) return true;
                return !inside(p+core::Vec3i{1,0,0}) || !inside(p+core::Vec3i{-1,0,0})
                    || !inside(p+core::Vec3i{0,0,1}) || !inside(p+core::Vec3i{0,0,-1})
                    || (sphere && (!inside(p+core::Vec3i{0,1,0}) || !inside(p+core::Vec3i{0,-1,0})));
            };
        } else {
            int layers = integer(extent);
            box = shapeBounds(center, layers - 1, layers - 1, 0, layers - 1);
            filter = [=](BlockSource&, core::Vec3i p) {
                p = p - center;
                int extent = layers - 1 - p.y;
                int distance = std::max(std::abs(p.x), std::abs(p.z));
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
    if (!maskText.empty()) {
        auto mask = edit::Mask::parse(maskText, player, error);
        if (!mask) throw std::runtime_error(error);
        filter = [shape = std::move(filter), mask = std::move(*mask)](BlockSource& region, core::Vec3i p) {
            return shape(region, p) && mask.test(region.getBlock({p.x, p.y, p.z}));
        };
    }
    auto job = edit::makeComputedEdit(player.getUuid(), player.getDimensionId(), box, std::move(*pattern), std::move(filter), std::string(name));
    submitEditJob(player, output, std::move(job), box.volume(), [pattern = std::string(patternText), mask = std::string(maskText)](Player& target) {
        if (pattern != "hand" && pattern != "offhand") rememberRecent(getSession(target).recentPatterns, pattern);
        if (!mask.empty()) rememberRecent(getSession(target).recentMasks, mask);
    });
}
void registerCommands(CommandRegistry& registry) {
    auto& registrar = ll::command::CommandRegistrar::getServerInstance();
    auto& root = registrar.getOrCreateCommand("cx", std::string(tr("command.cx")), CommandPermissionLevel::GameDirectors);
    auto run = [](AdvancedAction action, CommandOrigin const& origin, CommandOutput& output,
                   ll::command::Optional<CommandRawText> const& arguments) {
        if (auto* player = getCommandPlayer(origin, output)) {
            auto raw = arguments ? std::string_view(arguments->mText) : std::string_view{};
            executeAdvanced(*player, output, magic_enum::enum_name(action), raw);
        }
    };
    root.overload<AdvancedSubcommand>().required("action").optional("arguments").execute(
        [run](CommandOrigin const& origin, CommandOutput& output, AdvancedSubcommand const& args) {
            run(args.action, origin, output, args.arguments);
        }
    );

    if (!config.command.shortCommands) return;
    auto description = std::string(tr("command.cx"));
    for (auto action : magic_enum::enum_values<AdvancedAction>()) {
        auto name = std::string(magic_enum::enum_name(action));
        if (registry.findCommand(name)) continue;
        registrar.getOrCreateCommand(name, description, CommandPermissionLevel::GameDirectors)
            .overload<AdvancedArgs>()
            .optional("arguments")
            .execute([run, action](CommandOrigin const& origin, CommandOutput& output, AdvancedArgs const& args) {
                run(action, origin, output, args.arguments);
            });
    }
}
} // namespace
bool executeSchematic(Player& player, Feedback const& output, std::string_view action, std::string name, std::string format) {
    try {
        return runSchematic(player, output, action, std::move(name), std::move(format));
    } catch (std::exception const& error) {
        output.error(tr("advanced.failed", error.what()));
        return false;
    }
}

void executeAdvanced(Player& player,Feedback const& output,std::string_view operation,std::string_view arguments) {
    try {execute(player,output,operation,arguments);}
    catch(std::exception const& error) {output.error(tr("advanced.failed",error.what()));}
}
void enableAdvancedCommands() {
    listener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](auto& event) { registerCommands(event.commandRegistry()); },
        ll::event::EventPriority::Low
    );
    if (auto registry = ll::service::getCommandRegistry(false)) registerCommands(*registry);
}
void disableAdvancedCommands() {
    if (listener) ll::event::EventBus::getInstance().removeListener(listener);
    listener.reset();
}
} // namespace creatorx
