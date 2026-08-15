#include "render/frame/RenderView.h"

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

vkr::RenderView buildView(const vkr::RenderViewInput &input) {
    vkr::ShadowBuildInput shadowInput{};
    shadowInput.sceneLights = input.sceneLights;
    shadowInput.sceneBounds = input.sceneBounds;
    shadowInput.cameraView = input.view;
    shadowInput.cameraProjection = input.projection;
    shadowInput.cameraPosition = input.cameraPosition;
    shadowInput.cameraNearPlane = input.cameraNearPlane;
    shadowInput.cameraFarPlane = input.cameraFarPlane;
    shadowInput.fallbackSunDirection = input.defaultSun.direction;
    shadowInput.fallbackSunColor = input.defaultSun.color;
    shadowInput.fallbackSunIntensity = input.defaultSun.intensity;
    shadowInput.fallbackSunEnabled = input.fallbackSunEnabled;
    shadowInput.settings = input.settings;
    vkr::ShadowSystem shadowSystem;
    return vkr::buildRenderView(input, shadowSystem.build(shadowInput));
}

void testDefaultSunAndFrameData() {
    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.cameraPosition = {1.0f, 2.0f, 3.0f};
    input.ambientIntensity = -1.0f;
    input.defaultSun.intensity = 4.0f;

    const vkr::RenderView view = buildView(input);
    requireView(view.lightStats.directionalLights == 1,
                "default sun was not uploaded");
    requireView(view.lightStats.punctualLights == 0,
                "default view uploaded a punctual light");
    requireView(view.shadow.csm.enabled,
                "default sun did not produce a shadow view");
    requireView(view.globalUbo.cameraPosWS == glm::vec4(1.0f, 2.0f, 3.0f,
                                                        1.0f),
                "camera position was not packed into the frame UBO");
    requireView(view.globalUbo.ambientColorIntensity.w == 0.0f,
                "negative ambient intensity was not clamped");
    requireView(view.sceneLights[0].colorIntensity.w == 4.0f,
                "default sun intensity was not packed");
}

void testSceneWithoutDirectionalLightDisablesShadow() {
    std::vector<vkr::SceneLight> lights(1);
    lights[0].type = vkr::LightType::Point;

    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.sceneLights = &lights;
    const vkr::RenderView view = buildView(input);

    requireView(view.lightStats.directionalLights == 0,
                "a default sun was injected into a lit scene");
    requireView(view.lightStats.punctualLights == 1,
                "scene point light was not uploaded");
    requireView(!view.shadow.csm.enabled,
                "shadow stayed enabled without a directional light");
}

void testGpuLightLimits() {
    std::vector<vkr::SceneLight> lights;
    vkr::SceneLight shadowLight{};
    shadowLight.type = vkr::LightType::Directional;
    shadowLight.shadowPolicy = vkr::ShadowCastingPolicy::Forced;
    shadowLight.source = vkr::SceneLightSource::ExplicitEntity;
    shadowLight.stableKey = "entity/shadow";
    lights.push_back(shadowLight);
    for (uint32_t i = 0; i < vkr::kMaxSceneLights + 2; ++i) {
        vkr::SceneLight light{};
        light.type = vkr::LightType::Point;
        lights.push_back(light);
    }

    vkr::RenderViewInput input{};
    input.sceneBounds = validBounds();
    input.sceneLights = &lights;
    const vkr::RenderView view = buildView(input);

    requireView(view.lightStats.directionalLights == 1,
                "shadow-casting directional light was not retained");
    requireView(view.lightStats.pointLights == vkr::kMaxSceneLights - 1,
                "shared scene light limit was not enforced");
    requireView(view.lightStats.totalLights == vkr::kMaxSceneLights,
                "scene light upload count is incorrect");
    requireView(view.lightStats.ignoredLights == 3,
                "ignored light count is incorrect");
    requireView(view.lightStats.shadowCasterBufferIndex == 0,
                "shadow caster was not packed first");
}

void testEnvironmentFrameDataRequiresReadyResources() {
    vkr::RenderViewInput input{};
    input.settings.iblEnabled = true;
    input.settings.environmentIntensity = 2.5f;
    input.settings.environmentRotationRadians = 0.75f;
    input.maxSpecularLod = 8.0f;

    vkr::RenderView view = buildView(input);
    requireView(view.globalUbo.environmentParams.x == 0.0f,
                "IBL was enabled before environment resources were ready");

    input.environmentReady = true;
    view = buildView(input);
    requireView(
        view.globalUbo.environmentParams ==
            glm::vec4(1.0f, 2.5f, 0.75f, 8.0f),
        "environment settings were not packed into the frame UBO");
    requireView(view.globalUbo.inverseViewProjection == glm::mat4(1.0f),
                "inverse view-projection was not packed");
}

} // namespace

void runRenderViewTests() {
    testDefaultSunAndFrameData();
    testSceneWithoutDirectionalLightDisablesShadow();
    testGpuLightLimits();
    testEnvironmentFrameDataRequiresReadyResources();
}
