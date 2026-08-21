#include "DirectionalShadow.h"

#include "render/frame/SceneLight.h"
#include "scene_data/SceneTypes.h"
#include "render/geometry/BoundsMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

namespace vkr {

namespace {

bool buildReceiverCorners(const DirectionalShadowCameraData &camera,
                          std::array<glm::vec3, 8> &corners,
                          float &receiverDistance) {
    const float nearDistance = std::max(camera.nearPlane, 0.001f);
    receiverDistance = std::min(camera.farPlane, camera.shadowDistance);
    if (!std::isfinite(receiverDistance) ||
        receiverDistance <= nearDistance + 0.001f) {
        return false;
    }

    const glm::mat4 inverseProjection = glm::inverse(camera.projection);
    const glm::mat4 inverseView = glm::inverse(camera.view);
    uint32_t index = 0;
    for (uint32_t depthIndex = 0; depthIndex < 2; ++depthIndex) {
        const float distance = depthIndex == 0 ? nearDistance
                                              : receiverDistance;
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                const glm::vec2 ndc{x ? 1.0f : -1.0f,
                                    y ? 1.0f : -1.0f};
                glm::vec4 viewFar =
                    inverseProjection * glm::vec4(ndc, 1.0f, 1.0f);
                if (!std::isfinite(viewFar.w) ||
                    std::abs(viewFar.w) <= 1.0e-7f) {
                    return false;
                }
                viewFar /= viewFar.w;
                if (!std::isfinite(viewFar.z) ||
                    std::abs(viewFar.z) <= 1.0e-7f) {
                    return false;
                }
                const glm::vec3 viewPoint =
                    glm::vec3(viewFar) * (distance / -viewFar.z);
                const glm::vec4 worldPoint =
                    inverseView * glm::vec4(viewPoint, 1.0f);
                if (!std::isfinite(worldPoint.x) ||
                    !std::isfinite(worldPoint.y) ||
                    !std::isfinite(worldPoint.z)) {
                    return false;
                }
                corners[index++] = glm::vec3(worldPoint);
            }
        }
    }
    return true;
}

} // namespace

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled, const DirectionalShadowCameraData *camera,
    uint32_t shadowMapSize) {
    DirectionalShadowFrameData result{};
    if (!shadowsEnabled || !directionalLight || !bounds.valid ||
        shadowMapSize == 0 || bounds.radius <= 1.0e-4f) {
        return result;
    }

    glm::vec3 direction = directionalLight->directionWS;
    const float directionLength2 = glm::dot(direction, direction);
    if (directionLength2 <= 1.0e-8f)
        return result;
    direction = glm::normalize(direction);

    std::array<glm::vec3, 8> receiverCorners{};
    float receiverDistance = 0.0f;
    const bool receiverFitted =
        camera && buildReceiverCorners(*camera, receiverCorners,
                                       receiverDistance);
    if (!receiverFitted)
        receiverCorners = boundsCorners(bounds);
    glm::vec3 receiverCenter(0.0f);
    for (const glm::vec3 &corner : receiverCorners)
        receiverCenter += corner;
    receiverCenter /= static_cast<float>(receiverCorners.size());

    const float radius = std::max(bounds.radius, 0.1f);
    const glm::vec3 eye =
        receiverCenter + direction * (radius * 2.0f + 1.0f);
    const glm::vec3 primaryUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 fallbackUp(0.0f, 0.0f, 1.0f);
    const glm::vec3 up = std::abs(glm::dot(direction, primaryUp)) > 0.9999f
                             ? fallbackUp
                             : primaryUp;
    const glm::mat4 view = glm::lookAtRH(eye, receiverCenter, up);

    glm::vec2 receiverMin(std::numeric_limits<float>::max());
    glm::vec2 receiverMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3 &corner : receiverCorners) {
        const glm::vec3 lightSpace =
            glm::vec3(view * glm::vec4(corner, 1.0f));
        receiverMin = glm::min(receiverMin, glm::vec2(lightSpace));
        receiverMax = glm::max(receiverMax, glm::vec2(lightSpace));
    }

    float lightMinZ = std::numeric_limits<float>::max();
    float lightMaxZ = std::numeric_limits<float>::lowest();
    for (const glm::vec3 &corner : boundsCorners(bounds)) {
        const float z = (view * glm::vec4(corner, 1.0f)).z;
        lightMinZ = std::min(lightMinZ, z);
        lightMaxZ = std::max(lightMaxZ, z);
    }

    glm::vec2 halfExtent =
        glm::max((receiverMax - receiverMin) * 0.525f,
                 glm::vec2(0.05f));
    glm::vec2 center = (receiverMin + receiverMax) * 0.5f;
    const glm::vec2 worldUnitsPerTexel =
        (halfExtent * 2.0f) / static_cast<float>(shadowMapSize);
    center.x = std::round(center.x / worldUnitsPerTexel.x) *
               worldUnitsPerTexel.x;
    center.y = std::round(center.y / worldUnitsPerTexel.y) *
               worldUnitsPerTexel.y;

    const float depthRange = std::max(lightMaxZ - lightMinZ, 0.1f);
    const float depthPadding = std::max(depthRange * 0.1f, 0.05f);
    const float nearPlane = std::max(0.001f, -lightMaxZ - depthPadding);
    const float farPlane =
        std::max(nearPlane + 0.1f, -lightMinZ + depthPadding);

    glm::mat4 projection = glm::orthoRH_ZO(
        center.x - halfExtent.x, center.x + halfExtent.x,
        center.y - halfExtent.y, center.y + halfExtent.y, nearPlane,
        farPlane);
    projection[1][1] *= -1.0f;

    result.lightViewProjection = projection * view;
    result.lightView = view;
    result.lightSpaceMin =
        {center.x - halfExtent.x, center.y - halfExtent.y,
         lightMinZ - depthPadding};
    result.lightSpaceMax =
        {center.x + halfExtent.x, center.y + halfExtent.y,
         lightMaxZ + depthPadding};
    result.enabled = true;
    result.receiverFitted = receiverFitted;
    result.texelSize = 1.0f / static_cast<float>(shadowMapSize);
    result.receiverDistance = receiverFitted ? receiverDistance : 0.0f;
    return result;
}

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled, uint32_t shadowMapSize) {
    return buildDirectionalShadowFrameData(
        bounds, directionalLight, shadowsEnabled, nullptr, shadowMapSize);
}

// ---------------------------------------------------------------------------
// CSM: Cascade Shadow Maps
// ---------------------------------------------------------------------------

namespace {

bool buildFrustumCorners(const glm::mat4 &inverseView,
                         const glm::mat4 &inverseProjection,
                         float nearDist, float farDist,
                         std::array<glm::vec3, 8> &corners) {
    if (!std::isfinite(nearDist) || !std::isfinite(farDist) ||
        nearDist <= 0.001f || farDist <= nearDist + 0.001f) {
        return false;
    }
    uint32_t index = 0;
    for (uint32_t depthIndex = 0; depthIndex < 2; ++depthIndex) {
        const float distance =
            depthIndex == 0 ? nearDist : farDist;
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                const glm::vec2 ndc{x ? 1.0f : -1.0f,
                                    y ? 1.0f : -1.0f};
                glm::vec4 viewFar =
                    inverseProjection * glm::vec4(ndc, 1.0f, 1.0f);
                if (!std::isfinite(viewFar.w) ||
                    std::abs(viewFar.w) <= 1.0e-7f) {
                    return false;
                }
                viewFar /= viewFar.w;
                if (!std::isfinite(viewFar.z) ||
                    std::abs(viewFar.z) <= 1.0e-7f) {
                    return false;
                }
                const glm::vec3 viewPoint =
                    glm::vec3(viewFar) * (distance / -viewFar.z);
                corners[index++] =
                    glm::vec3(inverseView *
                              glm::vec4(viewPoint, 1.0f));
            }
        }
    }
    return true;
}

bool finiteVec3(const glm::vec3 &value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finiteMatrix(const glm::mat4 &matrix) {
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row]))
                return false;
        }
    }
    return true;
}

bool stableLightUp(const glm::vec3 &direction, glm::vec3 &up) {
    const glm::vec3 forward = -direction;
    const glm::vec3 absolute = glm::abs(forward);
    glm::vec3 helper(0.0f);
    if (absolute.x <= absolute.y && absolute.x <= absolute.z)
        helper.x = 1.0f;
    else if (absolute.y <= absolute.z)
        helper.y = 1.0f;
    else
        helper.z = 1.0f;

    const glm::vec3 right = glm::cross(forward, helper);
    const float rightLength2 = glm::dot(right, right);
    if (!std::isfinite(rightLength2) || rightLength2 <= 1.0e-8f)
        return false;
    const glm::vec3 normalizedRight = right / std::sqrt(rightLength2);
    up = glm::normalize(glm::cross(normalizedRight, forward));
    return finiteVec3(up);
}

} // namespace

std::array<float, kCsmCascadeCount> computeCsmSplits(
    float nearPlane, float farPlane, float shadowDistance, float lambda) {
    std::array<float, kCsmCascadeCount> splits{};
    const float effectiveFar =
        std::min(std::max(farPlane, nearPlane + 1.0f),
                 std::max(shadowDistance, nearPlane + 1.0f));
    const float nearClamped = std::max(nearPlane, 0.001f);
    const float farClamped = std::max(effectiveFar, nearClamped + 0.01f);

    for (uint32_t i = 0; i < kCsmCascadeCount; ++i) {
        const float ratio =
            static_cast<float>(i + 1) /
            static_cast<float>(kCsmCascadeCount);
        const float logSplit =
            nearClamped *
            std::pow(farClamped / nearClamped, ratio);
        const float uniformSplit = nearClamped + (farClamped - nearClamped) * ratio;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
    return splits;
}

CsmFrameData buildCsmFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled, const DirectionalShadowCameraData &camera,
    uint32_t shadowMapSize) {
    CsmFrameData result{};
    if (!shadowsEnabled || !directionalLight || !bounds.valid ||
        shadowMapSize == 0 || bounds.radius <= 1.0e-4f) {
        return result;
    }

    glm::vec3 direction = directionalLight->directionWS;
    const float directionLength2 = glm::dot(direction, direction);
    if (directionLength2 <= 1.0e-8f)
        return result;
    direction = glm::normalize(direction);
    result.lightDirection = direction;

    const auto splits = computeCsmSplits(
        camera.nearPlane, camera.farPlane,
        camera.shadowDistance, 0.75f);
    result.splitDepths = splits;

    const glm::mat4 inverseView = glm::inverse(camera.view);
    const glm::mat4 inverseProjection =
        glm::inverse(camera.projection);

    glm::vec3 up(0.0f);
    if (!stableLightUp(direction, up))
        return result;
    const glm::vec3 lightForward = -direction;
    const glm::vec3 lightRight = glm::normalize(
        glm::cross(lightForward, up));
    float cascadeNear = std::max(camera.nearPlane, 0.001f);
    const std::array<glm::vec3, 8> sceneCorners = boundsCorners(bounds);

    for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
        const float cascadeFar = splits[cascade];

        // Build frustum corners for this cascade's depth range
        std::array<glm::vec3, 8> frustumCorners{};
        if (!buildFrustumCorners(inverseView, inverseProjection,
                                 cascadeNear, cascadeFar,
                                 frustumCorners)) {
            return CsmFrameData{};
        }

        // A frustum-slice sphere is invariant under camera rotation. Keeping
        // the projection square avoids the light-space AABB breathing that
        // previously exposed receivers and casters near the viewport edge.
        glm::vec3 frustumCenter(0.0f);
        for (const glm::vec3 &corner : frustumCorners)
            frustumCenter += corner;
        frustumCenter /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3 &corner : frustumCorners)
            radius = std::max(radius, glm::length(corner - frustumCenter));
        if (!std::isfinite(radius) || radius <= 1.0e-4f)
            return CsmFrameData{};
        const float quantizedRadius =
            std::ceil(radius / kCsmRadiusQuantization) *
            kCsmRadiusQuantization;
        const float guardedRadius =
            std::max(quantizedRadius * (1.0f + kCsmGuardBandRatio), 0.05f);
        const float worldUnitsPerTexel =
            guardedRadius * 2.0f / static_cast<float>(shadowMapSize);
        if (!std::isfinite(worldUnitsPerTexel) ||
            worldUnitsPerTexel <= 1.0e-7f) {
            return CsmFrameData{};
        }

        // Snap in a fixed light basis. Snapping a lookAt-local origin would
        // always snap zero and would not stabilize world-space translation.
        const float centerRight = glm::dot(frustumCenter, lightRight);
        const float centerUp = glm::dot(frustumCenter, up);
        const float snappedRight =
            std::round(centerRight / worldUnitsPerTexel) *
            worldUnitsPerTexel;
        const float snappedUp =
            std::round(centerUp / worldUnitsPerTexel) *
            worldUnitsPerTexel;
        const glm::vec3 snappedCenter =
            frustumCenter + lightRight * (snappedRight - centerRight) +
            up * (snappedUp - centerUp);

        // Place the light-space eye beyond every receiver and scene-bound
        // caster. This prevents valid geometry from ending up behind the
        // synthetic orthographic camera as the view rotates.
        float maximumTowardLight = std::numeric_limits<float>::lowest();
        for (const glm::vec3 &corner : frustumCorners) {
            maximumTowardLight = std::max(
                maximumTowardLight,
                glm::dot(corner - snappedCenter, direction));
        }
        for (const glm::vec3 &corner : sceneCorners) {
            maximumTowardLight = std::max(
                maximumTowardLight,
                glm::dot(corner - snappedCenter, direction));
        }
        if (!std::isfinite(maximumTowardLight))
            return CsmFrameData{};
        const float eyeDistance =
            std::max(maximumTowardLight + std::max(bounds.radius * 0.1f, 0.5f),
                     1.0f);
        const glm::vec3 eye = snappedCenter + direction * eyeDistance;
        const glm::mat4 lightView =
            glm::lookAtRH(eye, snappedCenter, up);
        if (!finiteMatrix(lightView))
            return CsmFrameData{};

        float lightMinZ = std::numeric_limits<float>::max();
        float lightMaxZ = std::numeric_limits<float>::lowest();
        for (const glm::vec3 &corner : frustumCorners) {
            const glm::vec3 ls =
                glm::vec3(lightView * glm::vec4(corner, 1.0f));
            lightMinZ = std::min(lightMinZ, ls.z);
            lightMaxZ = std::max(lightMaxZ, ls.z);
        }

        // Also include scene bounds in Z range to catch occluders
        // behind the frustum
        for (const glm::vec3 &corner : sceneCorners) {
            const float z =
                (lightView * glm::vec4(corner, 1.0f)).z;
            lightMinZ = std::min(lightMinZ, z);
            lightMaxZ = std::max(lightMaxZ, z);
        }

        const float depthRange =
            std::max(lightMaxZ - lightMinZ, 0.1f);
        const float depthPadding =
            std::max(depthRange * 0.1f, 0.05f);
        const float nearPlaneLS =
            std::max(0.001f, -lightMaxZ - depthPadding);
        const float farPlaneLS =
            std::max(nearPlaneLS + 0.1f,
                     -lightMinZ + depthPadding);

        glm::mat4 proj = glm::orthoRH_ZO(
            -guardedRadius, guardedRadius, -guardedRadius, guardedRadius,
            nearPlaneLS, farPlaneLS);
        proj[1][1] *= -1.0f;
        const glm::mat4 lightViewProjection = proj * lightView;
        if (!finiteMatrix(lightViewProjection))
            return CsmFrameData{};

        CsmCascadeData &cascadeData = result.cascades[cascade];
        cascadeData.lightViewProjection = lightViewProjection;
        cascadeData.nearDistance = cascadeNear;
        cascadeData.splitDistance = cascadeFar;
        cascadeData.blendStartDistance =
            cascadeFar - (cascadeFar - cascadeNear) * kCsmBlendRatio;
        cascadeData.stableRadius = guardedRadius;
        cascadeData.worldUnitsPerTexel = worldUnitsPerTexel;
        cascadeData.texelSize =
            1.0f / static_cast<float>(shadowMapSize);
        cascadeData.valid = true;

        cascadeNear = cascadeFar;
    }

    result.enabled = true;
    return result;
}

} // namespace vkr
