#include "visual/SelectionOutline.h"

#include "core/region/CuboidSelection.h"
#include "mod/Global.h"
#include "net/Enhancements.h"

#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PrimitiveShapeDataPayload.h"
#include "mc/network/packet/PrimitiveShapesPacket.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/dimension/DimensionType.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <set>
#include <unordered_map>

namespace creatorx {
namespace {

// 每条棱使用固定 ID，续发和移动选区时更新已有图形。
constexpr uint64 selectionShapeIdBase  = 0x43524541544F5200;
constexpr float  selectionShapeSeconds = 2.0f;
std::unordered_map<mce::UUID,size_t> regionEdgeCounts;

PrimitiveShapeDataPayload selectionLine(uint64 edge, DimensionType dimension, Vec3 start, Vec3 end, mce::Color color) {
    LineDataPayload line{};
    line.mEndLocation = end;

    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId          = selectionShapeIdBase + edge;
    *shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Line;
    *shape.mLocation          = start;
    *shape.mRotation          = Vec3{0, 0, 0};
    *shape.mScale             = 1.0f;
    *shape.mColor             = color;
    *shape.mTimeLeftTotalSec  = selectionShapeSeconds;
    *shape.mDimensionId       = dimension;
    *shape.mMaxRenderDistance = config.visual.maxDistance;
    *shape.mExtraDataPayload  = std::move(line);
    return shape;
}

void showParticleEdge(
    Player const&        player,
    DimensionType        dimension,
    size_t               axis,
    std::array<float, 3> sample,
    double               begin,
    double               end,
    std::string_view     particle
) {
    constexpr double maxSegmentsPerEdge = 16;
    int              segments           = static_cast<int>(std::clamp(std::ceil(end - begin), 1.0, maxSegmentsPerEdge));
    for (int i = 0; i <= segments; ++i) {
        sample[axis] = static_cast<float>(std::lerp(begin, end, double(i) / segments));
        SpawnParticleEffectPacket packet(
            SpawnParticleEffectPacketPayload{
                Vec3{sample[0], sample[1], sample[2]},
                std::string(particle),
                static_cast<uchar>(int(dimension)),
                std::nullopt
        }
        );
        packet.sendTo(player);
    }
}

} // namespace

void showSelectionOutline(Player const& player, core::CuboidSelection const& selection, bool preview) {
    if (config.visual.mode == "none" || config.visual.maxDistance <= 0) {
        return;
    }
    auto const&           position = player.getPosition();
    std::array<double, 3> viewer{position.x, position.y, position.z};
    double                distance  = config.visual.maxDistance;
    auto                  dimension = player.getDimensionId();
    bool                  particles = config.visual.mode == "particles" || (config.visual.mode == "auto" && !hasEnhancedClient(player));
    std::vector<PrimitiveShapeDataPayload> shapes;
    if (!particles) shapes.reserve(18);

    auto color = preview ? mce::Color{255, 160, 40} : mce::Color{75, 255, 140};
    auto particle = preview ? std::string_view("minecraft:basic_flame_particle") : std::string_view(config.visual.particle);
    auto edge = [&](size_t id, size_t axis, std::array<float, 3> point, double begin, double end,
                    mce::Color edgeColor, std::string_view edgeParticle) {
        if (particles) {
            showParticleEdge(player, dimension, axis, point, begin, end, edgeParticle);
        } else if (begin < end) {
            point[axis] = float(begin);
            Vec3 start{point[0], point[1], point[2]};
            point[axis] = float(end);
            shapes.push_back(selectionLine(id, dimension, start, {point[0], point[1], point[2]}, edgeColor));
        }
    };

    size_t regionEdges=0;
    auto segment=[&](std::array<double,3> start,std::array<double,3> end) {
        // 将斜线也裁剪到玩家可视距离内，粒子和增强客户端共用同一轮廓。
        double length2=0,projection=0,offset2=0;
        std::array<double,3> delta{};
        for(size_t axis=0;axis<3;++axis) {
            delta[axis]=end[axis]-start[axis];
            auto offset=start[axis]-viewer[axis];
            length2+=delta[axis]*delta[axis];projection+=offset*delta[axis];offset2+=offset*offset;
        }
        if(length2<1e-9) return;
        double discriminant=projection*projection-length2*(offset2-distance*distance);
        if(discriminant<0) return;
        double begin=std::max(0.0,(-projection-std::sqrt(discriminant))/length2);
        double finish=std::min(1.0,(-projection+std::sqrt(discriminant))/length2);
        if(begin>finish) return;
        std::array<float,3> a{},b{};
        for(size_t axis=0;axis<3;++axis) {a[axis]=float(start[axis]+delta[axis]*begin);b[axis]=float(start[axis]+delta[axis]*finish);}
        if(particles) {
            int samples=int(std::clamp(std::ceil(std::sqrt(length2)*(finish-begin)),1.0,16.0));
            for(int i=0;i<=samples;++i) {
                float t=float(i)/samples;
                SpawnParticleEffectPacket packet(SpawnParticleEffectPacketPayload{
                    Vec3{std::lerp(a[0],b[0],t),std::lerp(a[1],b[1],t),std::lerp(a[2],b[2],t)},
                    std::string(particle),static_cast<uchar>(int(dimension)),std::nullopt});
                packet.sendTo(player);
            }
        } else shapes.push_back(selectionLine(128+regionEdges++,dimension,{a[0],a[1],a[2]},{b[0],b[1],b[2]},color));
    };
    auto bounds=selection.box();
    if(bounds && selection.type!=core::SelectionType::cuboid && !preview) {
        auto const& box=*bounds;
        if(selection.type==core::SelectionType::poly) {
            for(size_t i=0;i<selection.points.size();++i) {
                auto a=selection.points[i],b=selection.points[(i+1)%selection.points.size()];
                for(double y:{double(box.min.y),double(box.max.y)+1}) segment({a.x+0.5,y,a.z+0.5},{b.x+0.5,y,b.z+0.5});
                segment({a.x+0.5,double(box.min.y),a.z+0.5},{a.x+0.5,box.max.y+1.0,a.z+0.5});
            }
        } else if(selection.type==core::SelectionType::convex) {
            std::set<std::pair<size_t,size_t>> edges;
            for(auto face:selection.faces) for(size_t i=0;i<3;++i) edges.emplace(std::minmax(face[i],face[(i+1)%3]));
            for(auto [i,j]:edges) {
                auto a=selection.points[i],b=selection.points[j];
                segment({a.x+0.5,a.y+0.5,a.z+0.5},{b.x+0.5,b.y+0.5,b.z+0.5});
            }
        } else {
            auto c=*selection.first;
            std::array<double,3> center{c.x+0.5,c.y+0.5,c.z+0.5};
            std::array<double,3> radii{selection.radii.x+0.5,selection.radii.y+0.5,selection.radii.z+0.5};
            auto circle=[&](size_t a,size_t b,std::array<double,3> origin) {
                for(int i=0;i<32;++i) {
                    auto start=origin,end=origin;
                    double t=2*std::numbers::pi*i/32,u=2*std::numbers::pi*(i+1)/32;
                    start[a]+=radii[a]*std::cos(t);start[b]+=radii[b]*std::sin(t);
                    end[a]+=radii[a]*std::cos(u);end[b]+=radii[b]*std::sin(u);
                    segment(start,end);
                }
            };
            if(selection.type==core::SelectionType::cyl) {
                center[1]=box.min.y;circle(0,2,center);
                center[1]=box.max.y+1.0;circle(0,2,center);
                for(auto offset:{core::Vec3i{1,0,0},core::Vec3i{-1,0,0},core::Vec3i{0,0,1},core::Vec3i{0,0,-1}}) {
                    auto a=center,b=center;a[1]=box.min.y;
                    a[0]+=offset.x*radii[0];b[0]=a[0];a[2]+=offset.z*radii[2];b[2]=a[2];
                    segment(a,b);
                }
            } else {circle(0,1,center);circle(0,2,center);circle(1,2,center);}
        }
    }
    if (!preview) {
        auto previous=regionEdgeCounts[player.getUuid()];
        for(size_t i=regionEdges;i<previous;++i) {
            auto shape=selectionLine(128+i,dimension,{0,0,0},{0,0,0},color);
            *shape.mTimeLeftTotalSec=0.0f;shapes.push_back(std::move(shape));
        }
        if(regionEdges) regionEdgeCounts[player.getUuid()]=regionEdges;
        else regionEdgeCounts.erase(player.getUuid());
    }
    if (auto box = selection.type==core::SelectionType::cuboid || preview ? bounds : std::nullopt) {
        std::array<double, 3> low{double(box->min.x), double(box->min.y), double(box->min.z)};
        std::array<double, 3> high{box->max.x + 1.0, box->max.y + 1.0, box->max.z + 1.0};
        for (size_t axis = 0; axis < low.size(); ++axis) {
        auto sideA = (axis + 1) % low.size();
        auto sideB = (axis + 2) % low.size();
        for (size_t side = 0; side < 4; ++side) {
            auto aSide        = side & 1 ? high[sideA] : low[sideA];
            auto bSide        = side & 2 ? high[sideB] : low[sideB];
            auto offsetA      = aSide - viewer[sideA];
            auto offsetB      = bSide - viewer[sideB];
            auto reachSquared = distance * distance - offsetA * offsetA - offsetB * offsetB;
            if (reachSquared < 0) continue;

            // 保留玩家附近的线段，长棱的端点在远处时也能看到近处的边框。
            auto reach = std::sqrt(reachSquared);
            auto begin = std::max(low[axis], viewer[axis] - reach);
            auto end   = std::min(high[axis], viewer[axis] + reach);
            if (begin > end) continue;
            std::array<float, 3> sample{};
            sample[sideA] = static_cast<float>(aSide);
            sample[sideB] = static_cast<float>(bSide);
            edge(axis * 4 + side + (preview ? 16 : 0), axis, sample, begin, end, color, particle);
        }
        }
    }
    if (!preview) {
        for (size_t i = 0; i < 2; ++i) {
            auto const& corner = i == 0 ? selection.first : selection.second;
            if (!corner) continue;
            std::array<double, 3> center{corner->x + 0.5, corner->y + 1.15, corner->z + 0.5};
            double distanceSquared = 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                auto delta = center[axis] - viewer[axis];
                distanceSquared += delta * delta;
            }
            if (distanceSquared > distance * distance) continue;
            std::array<float, 3> point{float(center[0]), float(center[1]), float(center[2])};
            auto markerColor = i == 0 ? mce::Color{65, 175, 255} : mce::Color{255, 90, 120};
            auto markerParticle = i == 0 ? "minecraft:blue_flame_particle" : "minecraft:redstone_ore_dust_particle";
            double radius = i == 0 ? 0.85 : 0.65;
            for (size_t axis = 0; axis < 3; ++axis) {
                edge(32 + i * 3 + axis, axis, point, center[axis] - radius, center[axis] + radius, markerColor, markerParticle);
            }
        }
    }

    if (!shapes.empty()) {
        auto  packet     = MinecraftPackets::createPacket(MinecraftPacketIds::PrimitiveShapesPacket);
        auto& outline    = static_cast<PrimitiveShapesPacket&>(*packet);
        *outline.mShapes = std::move(shapes);
        outline.sendTo(player);
    }
}

void clearSelectionOutline(Player const& player, bool preview) {
    auto packet=MinecraftPackets::createPacket(MinecraftPacketIds::PrimitiveShapesPacket);
    auto& outline=static_cast<PrimitiveShapesPacket&>(*packet);
    std::vector<PrimitiveShapeDataPayload> shapes;
    auto clear = [&](uint64 edge) {
        auto shape=selectionLine(edge,player.getDimensionId(),{0,0,0},{0,0,0}, mce::Color{75, 255, 140});
        *shape.mTimeLeftTotalSec=0.0f;
        shapes.push_back(std::move(shape));
    };
    for (uint64 edge = 0; edge < 12; ++edge) clear(edge + (preview ? 16 : 0));
    if (!preview) for (uint64 edge = 32; edge < 38; ++edge) clear(edge);
    if(!preview) {
        auto it=regionEdgeCounts.find(player.getUuid());
        if(it!=regionEdgeCounts.end()) {
            for(size_t i=0;i<it->second;++i) clear(128+i);
            regionEdgeCounts.erase(it);
        }
    }
    *outline.mShapes=std::move(shapes);
    outline.sendTo(player);
}

void showBrushTarget(Player const& player, core::Vec3i position) {
    std::array<float, 3> low{position.x - 0.015f, position.y - 0.015f, position.z - 0.015f};
    auto dimension = player.getDimensionId();
    mce::Color color{90, 210, 255};
    constexpr float edgeLength = 1.03f;
    std::vector<PrimitiveShapeDataPayload> shapes;
    shapes.reserve(12);
    for (size_t axis = 0; axis < low.size(); ++axis) {
        for (size_t side = 0; side < 4; ++side) {
            auto point = low;
            if (side & 1) point[(axis + 1) % 3] += edgeLength;
            if (side & 2) point[(axis + 2) % 3] += edgeLength;
            Vec3 start{point[0], point[1], point[2]};
            point[axis] += edgeLength;
            auto shape = selectionLine(40 + axis * 4 + side, dimension, start, {point[0], point[1], point[2]}, color);
            *shape.mTimeLeftTotalSec = 0.35f;
            shapes.push_back(std::move(shape));
        }
    }
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PrimitiveShapesPacket);
    auto& outline = static_cast<PrimitiveShapesPacket&>(*packet);
    *outline.mShapes = std::move(shapes);
    outline.sendTo(player);
}

} // namespace creatorx
