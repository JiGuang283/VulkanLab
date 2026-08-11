#include "PunctualShadow.h"

#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

std::array<glm::mat4, kPointShadowFaceCount> pointShadowFaceViews(
    const glm::vec3 &lightPosition) {
    const glm::vec3 eye = lightPosition;
    // Right-handed lookAt for each cubemap face
    return {{
        // +X
        glm::lookAtRH(eye, eye + glm::vec3(1.0f, 0.0f, 0.0f),
                      glm::vec3(0.0f, -1.0f, 0.0f)),
        // -X
        glm::lookAtRH(eye, eye + glm::vec3(-1.0f, 0.0f, 0.0f),
                      glm::vec3(0.0f, -1.0f, 0.0f)),
        // +Y
        glm::lookAtRH(eye, eye + glm::vec3(0.0f, 1.0f, 0.0f),
                      glm::vec3(0.0f, 0.0f, 1.0f)),
        // -Y
        glm::lookAtRH(eye, eye + glm::vec3(0.0f, -1.0f, 0.0f),
                      glm::vec3(0.0f, 0.0f, -1.0f)),
        // +Z
        glm::lookAtRH(eye, eye + glm::vec3(0.0f, 0.0f, 1.0f),
                      glm::vec3(0.0f, -1.0f, 0.0f)),
        // -Z
        glm::lookAtRH(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f),
                      glm::vec3(0.0f, -1.0f, 0.0f)),
    }};
}

PointShadowData buildPointShadowData(const PointShadowLightInput &input,
                                     uint32_t layerBase,
                                     uint32_t /*shadowMapSize*/) {
    PointShadowData result{};
    result.position = input.position;
    result.farPlane = std::max(input.farPlane, 0.1f);
    result.layerBase = layerBase;

    const auto faceViews = pointShadowFaceViews(input.position);
    // A 90-degree field of view covers one cubemap face.
    const float fovY = glm::radians(90.0f);
    const float aspect = 1.0f;
    glm::mat4 proj = glm::perspectiveRH_ZO(fovY, aspect, 0.01f,
                                            result.farPlane);
    proj[1][1] *= -1.0f;

    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
        result.faceViewProjections[face] = proj * faceViews[face];

    result.enabled = true;
    return result;
}

SpotShadowData buildSpotShadowData(const SpotShadowLightInput &input,
                                   uint32_t layerBase,
                                   uint32_t /*shadowMapSize*/) {
    SpotShadowData result{};
    result.fovY = std::max(input.fovY, glm::radians(1.0f));
    result.nearPlane = std::max(input.nearPlane, 0.01f);
    result.farPlane = std::max(input.farPlane, result.nearPlane + 0.1f);
    result.layerBase = layerBase;

    glm::vec3 direction = glm::normalize(input.direction);
    const glm::vec3 zUp(0.0f, 0.0f, 1.0f);
    const glm::vec3 yUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 up =
        std::abs(glm::dot(direction, yUp)) > 0.9999f ? zUp : yUp;

    result.view =
        glm::lookAtRH(input.position, input.position + direction, up);
    glm::mat4 proj = glm::perspectiveRH_ZO(result.fovY, 1.0f,
                                            result.nearPlane,
                                            result.farPlane);
    proj[1][1] *= -1.0f;
    result.viewProjection = proj * result.view;
    result.enabled = true;
    return result;
}

} // namespace vkr
