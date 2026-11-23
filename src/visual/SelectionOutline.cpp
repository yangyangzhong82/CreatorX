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

namespace creatorx {
namespace {

// 每条棱使用固定 ID，续发和移动选区时更新已有图形。
constexpr uint64 selectionShapeIdBase  = 0x43524541544F5200;
constexpr float  selectionShapeSeconds = 2.0f;

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

    if (auto box = selection.box()) {
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
