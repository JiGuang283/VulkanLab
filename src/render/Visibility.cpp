#include "render/Visibility.h"

#include "diagnostics/Profiling.h"
#include "render/MaterialInstance.h"
#include "render/RenderView.h"
#include "render/ShadowVisibilityBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vkr {

namespace {

bool matrixNearlyEqual(const glm::mat4 &left, const glm::mat4 &right,
                       float epsilon = 1.0e-5f) {
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
            if (std::abs(left[column][row] - right[column][row]) > epsilon)
                return false;
        }
    }
    return true;
}

glm::vec3 cameraForward(const glm::mat4 &view) {
    const glm::mat4 inverseView = glm::inverse(view);
    const glm::vec3 forward =
        glm::vec3(inverseView * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    const float length = glm::length(forward);
    return std::isfinite(length) && length > 1.0e-6f
               ? forward / length
               : glm::vec3(0.0f, 0.0f, -1.0f);
}

bool isDefaultKey(const RenderItemKey &key) {
    return key.entityId.empty() && key.assetGeneration == 0 &&
           key.primitiveIndex == 0 && key.fallbackOrdinal == 0;
}

void sortOpaqueIndices(std::vector<RenderItemIndex> &indices,
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

void sortTransparentIndices(std::vector<RenderItemIndex> &indices,
                            const std::vector<RenderItem> &items,
                            const glm::vec3 &cameraPosition) {
    std::stable_sort(indices.begin(), indices.end(),
                     [&items, &cameraPosition](RenderItemIndex leftIndex,
                                               RenderItemIndex rightIndex) {
                         const glm::vec3 leftPosition(
                             items[leftIndex].world[3]);
                         const glm::vec3 rightPosition(
                             items[rightIndex].world[3]);
                         const float leftDistance = glm::dot(
                             leftPosition - cameraPosition,
                             leftPosition - cameraPosition);
                         const float rightDistance = glm::dot(
                             rightPosition - cameraPosition,
                             rightPosition - cameraPosition);
                         return leftDistance > rightDistance;
                     });
}

} // namespace

VisibilityFrame VisibilitySystem::build(std::vector<RenderItem> source,
                                        const RenderView &view,
                                        VkExtent2D viewportExtent,
                                        VisibilityBuildInput input) {
    VKL_PROFILE_ZONE("Build Visibility");
    VisibilityFrame result{};
    result.generation = nextVisibilityGeneration_++;
    result.items = std::move(source);

    const glm::mat4 cameraViewProjection =
        view.globalUbo.proj * view.globalUbo.view;
    const glm::mat4 stableCameraViewProjection =
        view.stableViewProjection;
    const glm::vec3 currentCameraPosition(view.globalUbo.cameraPosWS);
    const glm::vec3 currentCameraForward =
        cameraForward(view.globalUbo.view);

    std::string invalidationReason;
    if (!forcedInvalidationReason_.empty())
        invalidationReason = forcedInvalidationReason_;
    else if (!committed_)
        invalidationReason = "initial frame";
    else if (input.sceneGeneration != previousSceneGeneration_)
        invalidationReason = "scene generation changed";
    else if (input.cameraIdentity != previousCameraIdentity_)
        invalidationReason = "camera identity changed";
    else if (input.shaderIdentity != previousShaderIdentity_)
        invalidationReason = "shader variant changed";
    else if (viewportExtent.width != previousViewportExtent_.width ||
             viewportExtent.height != previousViewportExtent_.height)
        invalidationReason = "viewport resized";
    else if (!matrixNearlyEqual(view.stableProjection,
                                previousStableProjection_))
        invalidationReason = "projection changed";
    else {
        const float sceneRadius =
            input.sceneBounds.valid ? input.sceneBounds.radius : 0.0f;
        const float cutDistance = std::max(1.0f, sceneRadius * 0.25f);
        if (glm::distance(currentCameraPosition,
                          previousCameraPosition_) > cutDistance) {
            invalidationReason = "camera translation cut";
        } else {
            const float cosine = glm::clamp(
                glm::dot(currentCameraForward, previousCameraForward_),
                -1.0f, 1.0f);
            if (cosine < std::cos(glm::radians(45.0f)))
                invalidationReason = "camera rotation cut";
        }
    }

    const bool globalHistoryValid = invalidationReason.empty();
    if (!globalHistoryValid) {
        ++historyGeneration_;
        lastInvalidationReason_ = invalidationReason;
    }

    result.history.previousViewProjection =
        globalHistoryValid ? previousViewProjection_ : cameraViewProjection;
    result.history.currentViewProjection = cameraViewProjection;
    result.history.currentStableProjection = view.stableProjection;
    result.history.currentJitterPixels = view.projectionJitterPixels;
    result.history.previousJitterPixels =
        globalHistoryValid ? previousJitterPixels_
                           : view.projectionJitterPixels;
    result.history.cameraPosition = currentCameraPosition;
    result.history.cameraForward = currentCameraForward;
    result.history.viewportExtent = viewportExtent;
    result.history.sceneGeneration = input.sceneGeneration;
    result.history.historyGeneration = historyGeneration_;
    result.history.globalValid = globalHistoryValid;
    result.history.cameraIdentity = std::move(input.cameraIdentity);
    result.history.shaderIdentity = std::move(input.shaderIdentity);
    result.history.invalidationReason = globalHistoryValid
                                            ? std::string{}
                                            : invalidationReason;

    const CullingSettings &settings = view.settings.culling;
    const Frustum cameraFrustum =
        Frustum::fromVulkanClipMatrix(stableCameraViewProjection);

    result.cameraOpaque.reserve(result.items.size());
    result.cameraTransparent.reserve(result.items.size());
    for (uint32_t index = 0; index < result.items.size(); ++index) {
        RenderItem &item = result.items[index];
        if (item.sourceOrder == std::numeric_limits<uint32_t>::max())
            item.sourceOrder = index;
        item.primitiveIndex = item.key.primitiveIndex;
        item.materialIndex =
            item.material ? item.material->materialIndex() : 0u;
        if (isDefaultKey(item.key))
            item.key.fallbackOrdinal = item.sourceOrder + 1u;
        item.worldBounds = transformBounds(item.localBounds, item.world);

        const auto history = previousWorld_.find(item.key);
        item.historyValid = globalHistoryValid &&
                            history != previousWorld_.end();
        item.previousWorld = item.historyValid ? history->second : item.world;
        if (item.historyValid)
            ++result.history.historyValidItems;

        ++result.cpuStats.sourceDraws;
        bool cameraVisible = true;
        if (!item.worldBounds.valid) {
            ++result.cpuStats.invalidBounds;
        } else if (settings.frustumEnabled && cameraFrustum.valid &&
                   !cameraFrustum.intersects(item.worldBounds)) {
            ++result.cpuStats.frustumCulled;
            cameraVisible = false;
        } else if (settings.distanceEnabled &&
                   settings.maxDrawDistance > 0.0f &&
                   distanceSquaredToBounds(currentCameraPosition,
                                           item.worldBounds) >
                       settings.maxDrawDistance *
                           settings.maxDrawDistance) {
            ++result.cpuStats.distanceCulled;
            cameraVisible = false;
        } else if (settings.smallObjectEnabled &&
                   projectedBoundsAreSmallerThan(
                       item.worldBounds, stableCameraViewProjection,
                       viewportExtent,
                       settings.minProjectedSizePixels)) {
            ++result.cpuStats.smallObjectCulled;
            cameraVisible = false;
        }

        if (cameraVisible) {
            ++result.cpuStats.cameraVisible;
            if (item.queue == RenderQueueType::Opaque) {
                result.cameraOpaque.push_back(index);
                ++result.cpuStats.cameraOpaque;
            } else {
                result.cameraTransparent.push_back(index);
                ++result.cpuStats.cameraTransparent;
            }
        }

    }

    sortOpaqueIndices(result.cameraOpaque, result.items);
    sortTransparentIndices(result.cameraTransparent, result.items,
                           currentCameraPosition);
    ShadowVisibilityBuilder::build(result.items, view, settings, result);
    result.cpuStats.depthPrepassDraws =
        static_cast<uint32_t>(result.cameraOpaque.size());
    result.cpuStats.occlusionCandidates =
        static_cast<uint32_t>(result.cameraOpaque.size());
    return result;
}

void VisibilitySystem::commit(const VisibilityFrame &frame) {
    previousWorld_.clear();
    previousWorld_.reserve(frame.items.size());
    for (const RenderItem &item : frame.items)
        previousWorld_.insert_or_assign(item.key, item.world);
    previousViewProjection_ = frame.history.currentViewProjection;
    previousStableProjection_ =
        frame.history.currentStableProjection;
    previousJitterPixels_ = frame.history.currentJitterPixels;
    previousCameraPosition_ = frame.history.cameraPosition;
    previousCameraForward_ = frame.history.cameraForward;
    previousViewportExtent_ = frame.history.viewportExtent;
    previousSceneGeneration_ = frame.history.sceneGeneration;
    previousCameraIdentity_ = frame.history.cameraIdentity;
    previousShaderIdentity_ = frame.history.shaderIdentity;
    forcedInvalidationReason_.clear();
    committed_ = true;
}

void VisibilitySystem::invalidate(std::string reason) {
    forcedInvalidationReason_ = reason.empty() ? "explicit invalidation"
                                               : std::move(reason);
}

} // namespace vkr
