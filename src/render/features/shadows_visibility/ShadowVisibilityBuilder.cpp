#include "render/features/shadows_visibility/ShadowVisibilityBuilder.h"

#include "render/material/MaterialInstance.h"
#include "render/frame/RenderView.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "render/geometry/BoundsMath.h"

#include <algorithm>
#include <array>

namespace vkr {
namespace {

bool intersectsClipVolume(const Bounds &worldBounds,
                          const Frustum &frustum) {
    if (!worldBounds.valid)
        return true;
    return !frustum.valid || frustum.intersects(worldBounds);
}

void sortCasterQueue(std::vector<RenderItemIndex> &indices,
                     const std::vector<RenderItem> &items) {
    std::stable_sort(indices.begin(), indices.end(),
                     [&items](RenderItemIndex leftIndex,
                              RenderItemIndex rightIndex) {
                         const RenderItem &left = items[leftIndex];
                         const RenderItem &right = items[rightIndex];
                         const auto *leftTemplate =
                             left.material
                                 ? &left.material->materialTemplate()
                                 : nullptr;
                         const auto *rightTemplate =
                             right.material
                                 ? &right.material->materialTemplate()
                                 : nullptr;
                         if (leftTemplate != rightTemplate)
                             return leftTemplate < rightTemplate;
                         if (left.material != right.material)
                             return left.material < right.material;
                         if (left.mesh != right.mesh)
                             return left.mesh < right.mesh;
                         return left.sourceOrder < right.sourceOrder;
                     });
}

} // namespace

void ShadowVisibilityBuilder::build(
    const std::vector<RenderItem> &items, const RenderView &view,
    const CullingSettings &settings, VisibilityFrame &frame) {
    for (auto &queue : frame.directionalShadowCasters)
        queue.reserve(items.size());
    for (auto &lightQueues : frame.pointShadowCasters) {
        for (auto &queue : lightQueues)
            queue.reserve(items.size());
    }
    for (auto &queue : frame.spotShadowCasters)
        queue.reserve(items.size());

    const bool hasShadowViews =
        view.shadow.csm.enabled ||
        view.shadow.punctual.activePointCount > 0 ||
        view.shadow.punctual.activeSpotCount > 0;
    if (!hasShadowViews)
        return;

    std::array<Frustum, kCsmCascadeCount> directionalFrusta{};
    std::array<std::array<Frustum, kPointShadowFaceCount>,
               kMaxPointShadowLights>
        pointFrusta{};
    std::array<Frustum, kMaxSpotShadowLights> spotFrusta{};
    if (settings.shadowCullingEnabled) {
        if (view.shadow.csm.enabled) {
            for (uint32_t cascade = 0; cascade < kCsmCascadeCount;
                 ++cascade) {
                directionalFrusta[cascade] =
                    Frustum::fromVulkanClipMatrix(
                        view.shadow.csm.cascades[cascade]
                            .lightViewProjection);
            }
        }
        for (uint32_t light = 0;
             light < view.shadow.punctual.activePointCount; ++light) {
            const PointShadowData &point =
                view.shadow.punctual.points[light];
            if (!point.enabled)
                continue;
            for (uint32_t face = 0; face < kPointShadowFaceCount;
                 ++face) {
                pointFrusta[light][face] =
                    Frustum::fromVulkanClipMatrix(
                        point.faceViewProjections[face]);
            }
        }
        for (uint32_t light = 0;
             light < view.shadow.punctual.activeSpotCount; ++light) {
            const SpotShadowData &spot =
                view.shadow.punctual.spots[light];
            if (spot.enabled) {
                spotFrusta[light] = Frustum::fromVulkanClipMatrix(
                    spot.viewProjection);
            }
        }
    }

    for (uint32_t index = 0; index < items.size(); ++index) {
        const RenderItem &item = items[index];
        if (item.queue != RenderQueueType::Opaque)
            continue;
        ++frame.cpuStats.shadowCandidates;

        bool visibleInAnyShadow = false;
        if (view.shadow.csm.enabled) {
            for (uint32_t cascade = 0; cascade < kCsmCascadeCount;
                 ++cascade) {
                ++frame.cpuStats.directionalShadowCandidates[cascade];
                const bool visible =
                    !settings.shadowCullingEnabled ||
                    intersectsClipVolume(item.worldBounds,
                                         directionalFrusta[cascade]);
                if (!visible) {
                    ++frame.cpuStats.directionalShadowCulled[cascade];
                    continue;
                }
                frame.directionalShadowCasters[cascade].push_back(index);
                ++frame.cpuStats.directionalShadowDraws[cascade];
                visibleInAnyShadow = true;
            }
        }

        for (uint32_t light = 0;
             light < view.shadow.punctual.activePointCount; ++light) {
            const PointShadowData &point =
                view.shadow.punctual.points[light];
            if (!point.enabled)
                continue;
            const bool withinRange =
                !settings.shadowCullingEnabled ||
                !item.worldBounds.valid ||
                distanceSquaredToBounds(point.position,
                                        item.worldBounds) <=
                    point.farPlane * point.farPlane;
            if (!withinRange)
                continue;
            for (uint32_t face = 0; face < kPointShadowFaceCount;
                 ++face) {
                const bool visible =
                    !settings.shadowCullingEnabled ||
                    intersectsClipVolume(item.worldBounds,
                                         pointFrusta[light][face]);
                if (!visible)
                    continue;
                frame.pointShadowCasters[light][face].push_back(index);
                const uint32_t layer =
                    light * kPointShadowFaceCount + face;
                ++frame.cpuStats.pointShadowDraws[layer];
                visibleInAnyShadow = true;
            }
        }

        for (uint32_t light = 0;
             light < view.shadow.punctual.activeSpotCount; ++light) {
            const SpotShadowData &spot =
                view.shadow.punctual.spots[light];
            if (!spot.enabled)
                continue;
            const bool visible =
                !settings.shadowCullingEnabled ||
                intersectsClipVolume(item.worldBounds,
                                     spotFrusta[light]);
            if (!visible)
                continue;
            frame.spotShadowCasters[light].push_back(index);
            ++frame.cpuStats.spotShadowDraws[light];
            visibleInAnyShadow = true;
        }

        if (visibleInAnyShadow)
            ++frame.cpuStats.shadowVisible;
        else
            ++frame.cpuStats.shadowCulled;
    }

    for (auto &queue : frame.directionalShadowCasters)
        sortCasterQueue(queue, items);
    for (auto &lightQueues : frame.pointShadowCasters) {
        for (auto &queue : lightQueues)
            sortCasterQueue(queue, items);
    }
    for (auto &queue : frame.spotShadowCasters)
        sortCasterQueue(queue, items);
}

} // namespace vkr
