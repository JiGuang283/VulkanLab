#include "render/DirectionalShadow.h"
#include "scene/SceneLight.h"
#include "scene/SceneTypes.h"

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <stdexcept>

namespace {

void requireShadow(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

vkr::Bounds testBounds() {
    vkr::Bounds bounds;
    bounds.min = {-3.0f, -2.0f, -1.0f};
    bounds.max = {5.0f, 4.0f, 7.0f};
    bounds.center = (bounds.min + bounds.max) * 0.5f;
    bounds.radius = glm::length(bounds.max - bounds.center);
    bounds.valid = true;
    return bounds;
}

vkr::SceneLight testLight() {
    vkr::SceneLight light;
    light.type = vkr::LightType::Directional;
    light.directionWS = glm::normalize(glm::vec3(0.35f, 0.8f, 0.48f));
    return light;
}

void testBoundsFitInsideShadowClipVolume() {
    const vkr::Bounds bounds = testBounds();
    const vkr::SceneLight light = testLight();
    const auto shadow = vkr::buildDirectionalShadowFrameData(
        bounds, &light, true, 2048);
    requireShadow(shadow.enabled, "valid directional shadow fit was disabled");
    requireShadow(std::abs(shadow.texelSize - 1.0f / 2048.0f) < 1.0e-8f,
                  "shadow texel size is incorrect");

    constexpr float epsilon = 1.0e-4f;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                const glm::vec3 corner{
                    x ? bounds.max.x : bounds.min.x,
                    y ? bounds.max.y : bounds.min.y,
                    z ? bounds.max.z : bounds.min.z};
                const glm::vec4 clip =
                    shadow.lightViewProjection * glm::vec4(corner, 1.0f);
                requireShadow(std::abs(clip.w) > 1.0e-6f,
                              "shadow corner has an invalid clip w");
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                requireShadow(ndc.x >= -1.0f - epsilon &&
                                  ndc.x <= 1.0f + epsilon &&
                                  ndc.y >= -1.0f - epsilon &&
                                  ndc.y <= 1.0f + epsilon &&
                                  ndc.z >= -epsilon &&
                                  ndc.z <= 1.0f + epsilon,
                              "scene bounds escaped the shadow clip volume");
            }
        }
    }
}

void testShadowFitDisableConditions() {
    vkr::Bounds bounds = testBounds();
    vkr::SceneLight light = testLight();
    requireShadow(!vkr::buildDirectionalShadowFrameData(
                       bounds, &light, false, 2048)
                       .enabled,
                  "disabled shadows produced an active fit");
    requireShadow(!vkr::buildDirectionalShadowFrameData(
                       bounds, nullptr, true, 2048)
                       .enabled,
                  "missing directional light produced an active fit");
    bounds.valid = false;
    requireShadow(!vkr::buildDirectionalShadowFrameData(
                       bounds, &light, true, 2048)
                       .enabled,
                  "invalid scene bounds produced an active fit");
    bounds = testBounds();
    light.directionWS = glm::vec3(0.0f);
    requireShadow(!vkr::buildDirectionalShadowFrameData(
                       bounds, &light, true, 2048)
                       .enabled,
                  "zero light direction produced an active fit");
}

void testVerticalDirectionAndDeterministicSnapping() {
    const vkr::Bounds bounds = testBounds();
    vkr::SceneLight light = testLight();
    light.directionWS = {0.0f, 0.0f, 1.0f};
    const auto first = vkr::buildDirectionalShadowFrameData(
        bounds, &light, true, 1024);
    const auto second = vkr::buildDirectionalShadowFrameData(
        bounds, &light, true, 1024);
    requireShadow(first.enabled && second.enabled,
                  "vertical directional light fit failed");
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            requireShadow(std::isfinite(first.lightViewProjection[column][row]),
                          "shadow fit contains a non-finite matrix value");
            requireShadow(first.lightViewProjection[column][row] ==
                              second.lightViewProjection[column][row],
                          "shadow texel snapping is not deterministic");
        }
    }
}

} // namespace

void runDirectionalShadowTests() {
    testBoundsFitInsideShadowClipVolume();
    testShadowFitDisableConditions();
    testVerticalDirectionAndDeterministicSnapping();
}
