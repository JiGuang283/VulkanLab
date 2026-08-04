#include "DirectionalShadow.h"

#include "scene/SceneLight.h"
#include "scene/SceneTypes.h"
#include "scene/BoundsMath.h"

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
    const glm::vec3 zUp(0.0f, 0.0f, 1.0f);
    const glm::vec3 yUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 up = std::abs(glm::dot(direction, zUp)) > 0.95f ? yUp
                                                                    : zUp;
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

} // namespace vkr
