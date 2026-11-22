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

PrimitiveShapeDataPayload selectionLine(uint64 edge, DimensionType dimension, Vec3 start, Vec3 end) {
    LineDataPayload line{};
    line.mEndLocation = end;

    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId          = selectionShapeIdBase + edge;
    *shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Line;
    *shape.mLocation          = start;
    *shape.mRotation          = Vec3{0, 0, 0};
    *shape.mScale             = 1.0f;
    *shape.mColor             = mce::Color{75, 255, 140};
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
    double               end
) {
    constexpr double maxSegmentsPerEdge = 16;
    int              segments           = static_cast<int>(std::clamp(std::ceil(end - begin), 1.0, maxSegmentsPerEdge));
    for (int i = 0; i <= segments; ++i) {
        sample[axis] = static_cast<float>(std::lerp(begin, end, double(i) / segments));
        SpawnParticleEffectPacket packet(
            SpawnParticleEffectPacketPayload{
                Vec3{sample[0], sample[1], sample[2]},
                config.visual.particle,
                static_cast<uchar>(int(dimension)),
                std::nullopt
        }
        );
        packet.sendTo(player);
    }
}

} // namespace

void showSelectionOutline(Player const& player, core::CuboidSelection const& selection, bool preview) {
    if (config.visual.mode == "none" || config.visual.maxDistance <= 0 || !selection.first || !selection.second) {
        return;
    }

    auto const [a, b] = std::pair{*selection.first, *selection.second};
    std::array<double, 3> low{double(std::min(a.x, b.x)), double(std::min(a.y, b.y)), double(std::min(a.z, b.z))};
    std::array<double, 3> high{std::max(a.x, b.x) + 1.0, std::max(a.y, b.y) + 1.0, std::max(a.z, b.z) + 1.0};
    auto const&           position = player.getPosition();
    std::array<double, 3> viewer{position.x, position.y, position.z};
    double                distance  = config.visual.maxDistance;
    auto                  dimension = player.getDimensionId();
    bool                  particles = config.visual.mode == "particles" || (config.visual.mode == "auto" && !hasEnhancedClient(player));
    std::vector<PrimitiveShapeDataPayload> shapes;
    if (!particles) shapes.reserve(12);

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
            if (particles) {
                showParticleEdge(player, dimension, axis, sample, begin, end);
            } else if (begin < end) {
                sample[axis] = static_cast<float>(begin);
                Vec3 start{sample[0], sample[1], sample[2]};
                sample[axis] = static_cast<float>(end);
                Vec3 finish{sample[0], sample[1], sample[2]};
                shapes.push_back(selectionLine(axis * 4 + side + (preview ? 16 : 0), dimension, start, finish));
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
    for(uint64 edge=0;edge<12;++edge) {
        auto shape=selectionLine(edge+(preview ? 16 : 0),player.getDimensionId(),{0,0,0},{0,0,0});
        *shape.mTimeLeftTotalSec=0.0f;
        shapes.push_back(std::move(shape));
    }
    *outline.mShapes=std::move(shapes);
    outline.sendTo(player);
}

} // namespace creatorx
