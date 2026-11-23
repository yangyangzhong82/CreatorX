#include "edit/Clipboard.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "storage/Nbt.h"

namespace creatorx::edit {

void Clipboard::rememberStates(Cell cell) {
    for (auto block : {cell.block, cell.extra}) {
        if (block) mStates.try_emplace(block, *block->mSerializationId);
    }
}

namespace {

core::Vec3i rotate(core::Vec3i v,int turns,int axis) {
    for(int i=0;i<turns;++i) {
        if(axis==0) v={v.x,-v.z,v.y};
        else if(axis==1) v={-v.z,v.y,v.x};
        else v={-v.y,v.x,v.z};
    }
    return v;
}

double transformYaw(double yaw, int turns, int flipAxis, int axis) {
    if (axis == 1) yaw += 90 * turns;
    if (flipAxis == 0 || (axis == 2 && turns == 2)) yaw = -yaw;
    if (flipAxis == 2 || (axis == 0 && turns == 2)) yaw = 180 - yaw;
    yaw = std::fmod(yaw, 360);
    return yaw < 0 ? yaw + 360 : yaw;
}

Block const* transformBlock(Block const* block, int turns, int flipAxis, int axis) {
    if (!block) return nullptr;
    auto tag = *block->mSerializationId;
    if (!tag.contains("states")) return block;
    auto& states = tag["states"].get<CompoundTag>();
    if (axis != 1 && (turns & 1) && (states.contains("top_slot_bit") || states.contains("upside_down_bit")
        || states.contains("upper_block_bit") || states.contains("head_piece_bit"))) {
        throw std::runtime_error("this block has no sideways variant: "+block->getTypeName());
    }
    auto direction = [&](int value, int type) {
        static constexpr core::Vec3i facing[]{{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
        static constexpr core::Vec3i four[]{{0,0,1},{-1,0,0},{0,0,-1},{1,0,0}};
        static constexpr core::Vec3i stairs[]{{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
        auto values = type == 6 ? facing : type == 5 ? stairs : four;
        int count = type == 6 ? 6 : 4;
        if (value < 0 || value >= count) return value;
        auto v = values[value];
        v=rotate(v,turns,axis);
        if (flipAxis == 0) v.x = -v.x;
        if (flipAxis == 1) v.y = -v.y;
        if (flipAxis == 2) v.z = -v.z;
        for (int i = 0; i < count; ++i) if (values[i] == v) return i;
        throw std::runtime_error("block orientation cannot represent this rotation: "+block->getTypeName());
    };
    for (auto& [key, value] : states.mTags) {
        if (value.is_string()) {
            std::string original{std::string_view(value)};
            std::string changed = original;
            static constexpr std::string_view cardinals[]{"south","west","north","east"};
            bool six=key=="torch_facing_direction" || key=="minecraft:block_face" || key=="minecraft:facing_direction";
            if(six) {
                static constexpr std::string_view faces[]{"down","up","north","south","west","east"};
                auto name=original=="top" ? std::string_view("up") : std::string_view(original);
                for(int i=0;i<6;++i) if(name==faces[i]) {
                    auto index=direction(i,6);
                    if(key=="torch_facing_direction" && index==0) throw std::runtime_error("torch has no downward facing variant");
                    changed=key=="torch_facing_direction" && index==1 ? "top" : faces[index];
                }
            } else for (int i = 0; i < 4; ++i) if (original == cardinals[i]) changed = cardinals[direction(i,4)];
            bool invertY=flipAxis==1 || (axis!=1 && turns==2);
            if (original == "top" && invertY) changed = "bottom";
            if (original == "bottom" && invertY) changed = "top";
            if (original == "up" && invertY) changed = "down";
            if (original == "down" && invertY) changed = "up";
            if (key == "pillar_axis") {
                core::Vec3i v=original=="x" ? core::Vec3i{1,0,0} : original=="y" ? core::Vec3i{0,1,0} : core::Vec3i{0,0,1};
                v=rotate(v,turns,axis);
                changed=v.x ? "x" : v.y ? "y" : "z";
            }
            if(key=="lever_direction") {
                if(axis!=1 && (turns&1)) throw std::runtime_error("lever requires Y rotation or a half turn");
                if(turns&1) {
                    auto eastWest=changed.find("east_west"),northSouth=changed.find("north_south");
                    if(eastWest!=std::string::npos) changed.replace(eastWest,9,"north_south");
                    else if(northSouth!=std::string::npos) changed.replace(northSouth,11,"east_west");
                }
                if(invertY) {
                    if(changed.starts_with("down_")) changed.replace(0,5,"up_");
                    else if(changed.starts_with("up_")) changed.replace(0,3,"down_");
                }
            }
            value = changed;
        } else if (value.is_number()) {
            int original = int(value), changed = original;
            if (key == "facing_direction") changed = direction(original,6);
            if (key == "direction" || key == "coral_direction") changed = direction(original,4);
            if(key=="vine_direction_bits") {
                changed=0;
                for(int i=0;i<4;++i) if(original&(1<<i)) changed|=1<<direction(i,4);
            }
            if (key == "weirdo_direction") changed = direction(original,5);
            if (key == "ground_sign_direction") {
                if(axis!=1 && turns) throw std::runtime_error("standing signs require Y rotation");
                changed = int(transformYaw(original * 22.5, turns, flipAxis, axis) / 22.5);
            }
            if(key=="rail_direction") {
                if(axis!=1 && turns) throw std::runtime_error("rails require Y rotation");
                static constexpr int rotateRail[]{1,0,5,4,2,3,7,8,9,6};
                static constexpr int flipX[]{0,1,3,2,4,5,7,6,9,8};
                static constexpr int flipZ[]{0,1,2,3,5,4,9,8,7,6};
                if(original>=0 && original<10) {
                    for(int i=0;i<turns;++i) changed=rotateRail[changed];
                    if(flipAxis==0) changed=flipX[changed];
                    if(flipAxis==2) changed=flipZ[changed];
                    if(flipAxis==1 && changed>=2 && changed<=5) changed^=1;
                }
            }
            if ((key == "upside_down_bit" || key == "top_slot_bit" || key == "upper_block_bit") && (flipAxis == 1 || (axis!=1 && turns==2))) changed = !original;
            if (key == "door_hinge_bit" && (flipAxis == 0 || flipAxis == 2)) changed = !original;
            if (value.hold<ByteTag>()) value = ByteTag(uchar(changed));
            else value = changed;
        }
    }
    auto transformed = Block::tryGetFromRegistry(tag);
    if (!transformed) throw std::runtime_error("block state cannot be transformed: " + block->getTypeName());
    return transformed.as_ptr();
}

template <class Transform, class Linear>
std::shared_ptr<Clipboard const> transform(Clipboard const& source, core::Vec3i newSize, Transform&& map, Linear&& linear, int turns, int flipAxis,int axis=1) {
    auto result   = std::make_shared<Clipboard>();
    result->size  = newSize;
    auto correction = map(core::Vec3i{0,0,0});
    result->offset = linear(source.offset) - correction;
    result->origin = source.origin - source.offset + result->offset;
    std::unordered_map<Block const*, Block const*> blocks;
    auto block = [&](Block const* b) {
        auto [it, added] = blocks.try_emplace(b);
        if (added) it->second = transformBlock(b, turns, flipAxis,axis);
        return it->second;
    };
    for (int y = 0; y < source.size.y; ++y) {
        for (int z = 0; z < source.size.z; ++z) {
            for (int x = 0; x < source.size.x; ++x) {
                auto old = core::Vec3i{x, y, z};
                auto dst = map(old);
                auto cell = source.at(source.index(old));
                result->store(result->index(dst), {block(cell.block), block(cell.extra)});
                auto entity = source.entities.find(source.index(old));
                if (entity != source.entities.end()) {
                    auto tag = entity->second;
                    if (cell.block && cell.block->getBlockType().mBlockEntityType == BlockActorType::Skull
                        && tag.contains("Rotation") && tag["Rotation"].is_number() && (axis == 1 || !(turns & 1))) {
                        tag["Rotation"] = float(transformYaw(double(tag["Rotation"]), turns, flipAxis, axis));
                    }
                    if(tag.contains("pairx") && tag.contains("pairz") && tag.contains("x") && tag.contains("z")) {
                        auto pair=map(core::Vec3i{old.x+int(tag["pairx"])-int(tag["x"]),old.y,old.z+int(tag["pairz"])-int(tag["z"])});
                        tag["pairx"]=pair.x; tag["pairz"]=pair.z;
                    }
                    tag["x"] = dst.x; tag["y"] = dst.y; tag["z"] = dst.z;
                    result->entities.emplace(result->index(dst), std::move(tag));
                }
            }
        }
    }
    for(auto actor:source.actors) {
        if(actor.contains("blockPos")) actor["blockPos"]=storage::vector(map(storage::vector(actor["blockPos"])));
        if(actor.contains("pos")) {
            auto const& pos=actor["pos"].get<ListTag>();
            if(pos.size()==3) {
                auto ex=linear({1,0,0}), ey=linear({0,1,0}), ez=linear({0,0,1});
                double x=double(pos[0]), y=double(pos[1]), z=double(pos[2]);
                auto point=storage::Nbt::array({
                    double(ex.x*x+ey.x*y+ez.x*z+correction.x+(1-ex.x-ey.x-ez.x)*0.5),
                    double(ex.y*x+ey.y*y+ez.y*z+correction.y+(1-ex.y-ey.y-ez.y)*0.5),
                    double(ex.z*x+ey.z*y+ez.z*z+correction.z+(1-ex.z-ey.z-ez.z)*0.5)});
                actor["pos"]=point;
                if(actor.contains("nbt")) actor["nbt"]["Pos"]=point;
            }
        }
        if(actor.contains("nbt") && actor["nbt"].contains("Rotation")) {
            auto const& rotation=actor["nbt"]["Rotation"].get<ListTag>();
            if(rotation.size()==2) {
                double radians=std::numbers::pi/180;
                double oldYaw=double(rotation[0])*radians,oldPitch=double(rotation[1])*radians;
                double x=-std::sin(oldYaw)*std::cos(oldPitch),y=-std::sin(oldPitch),z=std::cos(oldYaw)*std::cos(oldPitch);
                auto ex=linear({1,0,0}),ey=linear({0,1,0}),ez=linear({0,0,1});
                double dx=ex.x*x+ey.x*y+ez.x*z,dy=ex.y*x+ey.y*y+ez.y*z,dz=ex.z*x+ey.z*y+ez.z*z;
                float yaw=float(std::atan2(-dx,dz)/radians),pitch=float(std::atan2(-dy,std::hypot(dx,dz))/radians);
                actor["nbt"]["Rotation"]=storage::Nbt::array({yaw,pitch});
            }
        }
        result->actors.push_back(std::move(actor));
    }
    return result;
}

} // namespace

std::shared_ptr<Clipboard const> rotateClipboard(
    std::shared_ptr<Clipboard const> const& source,
    int quarterTurns, int axis
) {
    if (!source) return nullptr;
    quarterTurns %= 4;
    if (quarterTurns < 0) quarterTurns += 4;
    if (quarterTurns == 0) return source;
    auto linear=[=](core::Vec3i p) {return rotate(p,quarterTurns,axis);};
    auto corner=linear(source->size-core::Vec3i{1,1,1});
    core::Vec3i low{std::min(corner.x,0),std::min(corner.y,0),std::min(corner.z,0)};
    core::Vec3i size{std::abs(corner.x)+1,std::abs(corner.y)+1,std::abs(corner.z)+1};
    auto map=[=](core::Vec3i p) {return linear(p)-low;};
    return transform(*source,size,map,linear,quarterTurns,-1,axis);
}

std::shared_ptr<Clipboard const> flipClipboard(
    std::shared_ptr<Clipboard const> const& source,
    core::Direction direction
) {
    if (!source) return nullptr;
    auto size = source->size;
    auto map = [size, direction](core::Vec3i p) {
        switch (direction) {
        case core::Direction::east:
        case core::Direction::west: p.x = size.x - 1 - p.x; break;
        case core::Direction::up:
        case core::Direction::down: p.y = size.y - 1 - p.y; break;
        case core::Direction::north:
        case core::Direction::south: p.z = size.z - 1 - p.z; break;
        case core::Direction::me: break;
        }
        return p;
    };
    int axis = direction == core::Direction::east || direction == core::Direction::west ? 0 : direction == core::Direction::up || direction == core::Direction::down ? 1 : 2;
    auto linear = [axis](core::Vec3i p) {
        if (axis == 0) p.x = -p.x;
        if (axis == 1) p.y = -p.y;
        if (axis == 2) p.z = -p.z;
        return p;
    };
    return transform(*source, size, map, linear, 0, axis);
}

} // namespace creatorx::edit
