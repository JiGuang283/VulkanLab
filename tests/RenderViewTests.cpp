#include "render/RenderView.h"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

void requireView(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

vkr::Bounds validBounds() {
    vkr::Bounds bounds{};
    bounds.min = {-2.0f, -1.0f, -3.0f};
    bounds.max = {2.0f, 3.0f, 1.0f};
    bounds.center = (bounds.min + bounds.max) * 0.5f;
    bounds.radius = glm::length(bounds.max - bounds.center);
    bounds.valid = true;
    return bounds;
}

void testDefaultSunAndFrameData() {
    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.cameraPosition = {1.0f, 2.0f, 3.0f};
    input.ambientIntensity = -1.0f;
    input.defaultSun.intensity = 4.0f;

    const vkr::RenderView view = vkr::buildRenderView(input);
    requireView(view.lightStats.directionalLights == 1,
                "default sun was not uploaded");
    requireView(view.lightStats.punctualLights == 0,
                "default view uploaded a punctual light");
    requireView(view.directionalShadow.enabled,
                "default sun did not produce a shadow view");
    requireView(view.globalUbo.cameraPosWS == glm::vec4(1.0f, 2.0f, 3.0f,
                                                        1.0f),
                "camera position was not packed into the frame UBO");
    requireView(view.globalUbo.ambientColorIntensity.w == 0.0f,
                "negative ambient intensity was not clamped");
    requireView(view.globalUbo.directionalLights[0].colorIntensity.w == 4.0f,
                "default sun intensity was not packed");
}

void testSceneWithoutDirectionalLightDisablesShadow() {
    std::vector<vkr::SceneLight> lights(1);
    lights[0].type = vkr::LightType::Point;

    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.sceneLights = &lights;
    const vkr::RenderView view = vkr::buildRenderView(input);

    requireView(view.lightStats.directionalLights == 0,
                "a default sun was injected into a lit scene");
    requireView(view.lightStats.punctualLights == 1,
                "scene point light was not uploaded");
    requireView(!view.directionalShadow.enabled,
                "shadow stayed enabled without a directional light");
}

void testGpuLightLimits() {
    std::vector<vkr::SceneLight> lights;
    for (uint32_t i = 0; i < vkr::kMaxDirectionalLights + 1; ++i) {
        vkr::SceneLight light{};
        light.type = vkr::LightType::Directional;
        lights.push_back(light);
    }
    for (uint32_t i = 0; i < vkr::kMaxPunctualLights + 2; ++i) {
        vkr::SceneLight light{};
        light.type = vkr::LightType::Point;
        lights.push_back(light);
    }

    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.sceneLights = &lights;
    const vkr::RenderView view = vkr::buildRenderView(input);

    requireView(view.lightStats.directionalLights ==
                    vkr::kMaxDirectionalLights,
                "directional light limit was not enforced");
    requireView(view.lightStats.punctualLights == vkr::kMaxPunctualLights,
                "punctual light limit was not enforced");
    requireView(view.lightStats.ignoredLights == 3,
                "ignored light count is incorrect");
}

} // namespace

void runRenderViewTests() {
    testDefaultSunAndFrameData();
    testSceneWithoutDirectionalLightDisablesShadow();
    testGpuLightLimits();
}
