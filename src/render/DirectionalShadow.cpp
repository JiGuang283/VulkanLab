#include "DirectionalShadow.h"

#include "scene/SceneLight.h"
#include "scene/SceneTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

namespace vkr {

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled, uint32_t shadowMapSize) {
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

    const float radius = std::max(bounds.radius, 0.1f);
    const glm::vec3 eye = bounds.center + direction * (radius * 2.0f + 1.0f);
    const glm::vec3 zUp(0.0f, 0.0f, 1.0f);
    const glm::vec3 yUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 up = std::abs(glm::dot(direction, zUp)) > 0.95f ? yUp
                                                                    : zUp;
    const glm::mat4 view = glm::lookAtRH(eye, bounds.center, up);

    glm::vec3 lightMin(std::numeric_limits<float>::max());
    glm::vec3 lightMax(std::numeric_limits<float>::lowest());
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                const glm::vec3 corner{
                    x ? bounds.max.x : bounds.min.x,
                    y ? bounds.max.y : bounds.min.y,
                    z ? bounds.max.z : bounds.min.z};
                const glm::vec3 lightSpace =
                    glm::vec3(view * glm::vec4(corner, 1.0f));
                lightMin = glm::min(lightMin, lightSpace);
                lightMax = glm::max(lightMax, lightSpace);
            }
        }
    }

    glm::vec2 halfExtent =
        glm::max((glm::vec2(lightMax) - glm::vec2(lightMin)) * 0.525f,
                 glm::vec2(0.05f));
    glm::vec2 center = (glm::vec2(lightMin) + glm::vec2(lightMax)) * 0.5f;
    const glm::vec2 worldUnitsPerTexel =
        (halfExtent * 2.0f) / static_cast<float>(shadowMapSize);
    center.x = std::round(center.x / worldUnitsPerTexel.x) *
               worldUnitsPerTexel.x;
    center.y = std::round(center.y / worldUnitsPerTexel.y) *
               worldUnitsPerTexel.y;

    const float depthRange = std::max(lightMax.z - lightMin.z, 0.1f);
    const float depthPadding = std::max(depthRange * 0.1f, 0.05f);
    const float nearPlane = std::max(0.001f, -lightMax.z - depthPadding);
    const float farPlane =
        std::max(nearPlane + 0.1f, -lightMin.z + depthPadding);

    glm::mat4 projection = glm::orthoRH_ZO(
        center.x - halfExtent.x, center.x + halfExtent.x,
        center.y - halfExtent.y, center.y + halfExtent.y, nearPlane,
        farPlane);
    projection[1][1] *= -1.0f;

    result.lightViewProjection = projection * view;
    result.enabled = true;
    result.texelSize = 1.0f / static_cast<float>(shadowMapSize);
    return result;
}

} // namespace vkr
