#include "Application.h"
#include "UniformData.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"
#include "core/ResourcePoolSelfTest.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "render/GuiSystem.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTextureSlot.h"
#include "render/PipelineCache.h"
#include "render/Renderer.h"
#include "scene/SceneFactory.h"
#include "scene/SceneLight.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

namespace {

glm::vec3 normalizeOrFallback(const glm::vec3 &v,
                              const glm::vec3 &fallback) {
    const float len2 = glm::dot(v, v);
    if (len2 <= 1.0e-6f)
        return glm::normalize(fallback);
    return glm::normalize(v);
}

SceneLight makeDefaultSun(const glm::vec3 &direction, const glm::vec3 &color,
                          float intensity) {
    SceneLight light{};
    light.type = LightType::Directional;
    light.directionWS =
        normalizeOrFallback(direction, glm::vec3(0.3f, 0.8f, 0.5f));
    light.color = color;
    light.intensity = std::max(intensity, 0.0f);
    return light;
}

GpuLight makeGpuLight(const SceneLight &light) {
    GpuLight gpu{};
    const glm::vec3 direction = normalizeOrFallback(
        light.directionWS,
        light.type == LightType::Directional ? glm::vec3(0.3f, 0.8f, 0.5f)
                                             : glm::vec3(0.0f, -1.0f, 0.0f));

    float innerConeCos = glm::clamp(light.innerConeCos, -1.0f, 1.0f);
    float outerConeCos = glm::clamp(light.outerConeCos, -1.0f, 1.0f);
    if (light.type == LightType::Spot && innerConeCos < outerConeCos)
        std::swap(innerConeCos, outerConeCos);

    gpu.positionRange =
        glm::vec4(light.positionWS, std::max(light.range, 0.0f));
    gpu.directionInnerCos = glm::vec4(direction, innerConeCos);
    gpu.colorIntensity =
        glm::vec4(glm::max(light.color, glm::vec3(0.0f)),
                  std::max(light.intensity, 0.0f));
    gpu.params =
        glm::vec4(static_cast<float>(static_cast<uint32_t>(light.type)),
                  outerConeCos, 0.0f, 0.0f);
    return gpu;
}

const char *alphaModeName(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Opaque:
        return "Opaque";
    case AlphaMode::Mask:
        return "Mask";
    case AlphaMode::Blend:
        return "Blend";
    }
    return "Unknown";
}

const char *slotName(MaterialTextureSlot slot) {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
        return "BaseColor";
    case MaterialTextureSlot::Normal:
        return "Normal";
    case MaterialTextureSlot::MetallicRoughness:
        return "MetallicRoughness";
    case MaterialTextureSlot::Occlusion:
        return "Occlusion";
    case MaterialTextureSlot::Emissive:
        return "Emissive";
    case MaterialTextureSlot::Count:
        break;
    }
    return "Unknown";
}

bool isTransparentMaterial(const MaterialParams &params) {
    return params.alphaMode == AlphaMode::Blend ||
           params.transmissionFactor > 0.0f;
}

} // namespace

Application::Application(const Config &config) : config_(config) {}

Application::~Application() {
    if (device_)
        vkDeviceWaitIdle(device_->logicalDevice());
}

void Application::run() {
    init();
    mainLoop();
}

void Application::registerScene(SceneEntry entry) {
    sceneRegistry_.push_back(std::move(entry));
}

void Application::init() {
#ifndef NDEBUG
    runResourcePoolSelfTest();
#endif

    shaderVariants_.assign(kShaderVariants.begin(), kShaderVariants.end());

    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions));
    device_ = std::make_unique<Device>(*context_);
    descriptorAllocator_ = std::make_unique<DescriptorAllocator>(*device_);
    swapChain_ =
        std::make_unique<SwapChain>(*device_, context_->surface(), [this]() {
            return window_->framebufferExtent();
        });
    frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, *frameSync_, *descriptorAllocator_,
        sizeof(GlobalUBO));

    window_->setResizeCallback(
        [this](int, int) { frameSync_->notifyResize(); });

    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));

    if (sceneRegistry_.empty())
        throw std::runtime_error("No scenes registered; call "
                                 "Application::registerScene before run().");

    const int start = std::clamp(config_.defaultSceneIndex, 0,
                                 static_cast<int>(sceneRegistry_.size()) - 1);
    currentScene_ = sceneRegistry_[start].factory(
        *device_, *frameSync_, *descriptorAllocator_);
    currentSceneIndex_ = start;

    pipelineCache_ = std::make_unique<PipelineCache>(*device_);

    if (currentScene_->initialCamera) {
        const auto &p = *currentScene_->initialCamera;
        camera_.setPosition(p.position);
        camera_.setYawPitch(p.yaw, p.pitch);
    }

    // ImGui on top of the main render pass.
    gui_ = std::make_unique<GuiSystem>(
        context_->instance(), *device_, renderer_->renderPass(),
        window_->handle(), swapChain_->imageCount(), swapChain_->imageCount());
}

void Application::switchScene(int index) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return;
    if (index == currentSceneIndex_)
        return;

    vkDeviceWaitIdle(device_->logicalDevice());
    pipelineCache_->clear();
    currentScene_.reset();

    const auto &entry = sceneRegistry_[index];
    currentScene_ =
        entry.factory(*device_, *frameSync_, *descriptorAllocator_);
    currentSceneIndex_ = index;

    if (currentScene_->initialCamera) {
        const auto &p = *currentScene_->initialCamera;
        camera_.setPosition(p.position);
        camera_.setYawPitch(p.yaw, p.pitch);
    }

    VKR_LOG_INFO("Scene", "Switched to {}", entry.name);
}

void Application::updateInputMode() {
    auto &io = ImGui::GetIO();

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        const bool overUI = io.WantCaptureMouse || ImGui::IsAnyItemActive();
        if (pressed && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::UI;
        }
    }
}

void Application::processCameraInput(float dt) {
    glm::vec3 move{0.0f};
    if (input_->isKeyDown(Key::W))
        move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))
        move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))
        move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))
        move.x += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::Q))
        move.y -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::E))
        move.y += config_.moveSpeed * dt;
    camera_.translate(move);

    const auto d = input_->mouseDelta();
    camera_.rotate(-d.x * config_.mouseSensitivity,
                   -d.y * config_.mouseSensitivity);
}

void Application::updateUniforms(uint32_t frameIndex) {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    ubo.cameraPosWS = glm::vec4(camera_.position(), 1.0f);
    ubo.ambientColorIntensity =
        glm::vec4(glm::max(ambientColor_, glm::vec3(0.0f)),
                  std::max(ambientIntensity_, 0.0f));

    uint32_t directionalCount = 0;
    uint32_t punctualCount = 0;
    uint32_t ignoredCount = 0;

    const auto uploadLight = [&](const SceneLight &light) {
        switch (light.type) {
        case LightType::Directional:
            if (directionalCount < kMaxDirectionalLights) {
                ubo.directionalLights[directionalCount++] =
                    makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        case LightType::Point:
        case LightType::Spot:
            if (punctualCount < kMaxPunctualLights) {
                ubo.punctualLights[punctualCount++] = makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        }
    };

    const auto *sceneLights =
        currentScene_ ? &currentScene_->lights() : nullptr;
    if (sceneLights && !sceneLights->empty()) {
        for (const auto &light : *sceneLights)
            uploadLight(light);
    } else {
        uploadLight(makeDefaultSun(defaultSunDirection_, defaultSunColor_,
                                   defaultSunIntensity_));
    }

    ubo.lightCounts =
        glm::vec4(static_cast<float>(directionalCount),
                  static_cast<float>(punctualCount), 0.0f, 0.0f);
    lastUploadedDirectionalLights_ = directionalCount;
    lastUploadedPunctualLights_ = punctualCount;
    if (ignoredCount != lastIgnoredLights_) {
        if (ignoredCount > 0) {
            VKR_LOG_WARN("Lighting",
                         "Ignored {} scene lights beyond GPU light limits.",
                         ignoredCount);
        }
        lastIgnoredLights_ = ignoredCount;
    }
    std::memcpy(renderer_->mappedUniformBuffer(frameIndex), &ubo, sizeof(ubo));
}

void Application::drawGui() {
    ImGui::Begin("Renderer");
    if (!shaderVariants_.empty()) {
        const char *current =
            shaderVariants_[currentShaderVariantIndex_].displayName;
        if (ImGui::BeginCombo("Shader", current)) {
            for (int i = 0; i < static_cast<int>(shaderVariants_.size());
                 ++i) {
                const bool selected = (i == currentShaderVariantIndex_);
                if (ImGui::Selectable(shaderVariants_[i].displayName,
                                      selected)) {
                    currentShaderVariantIndex_ = i;
                    VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                                 shaderVariants_[i].displayName);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::End();

    ImGui::Begin("Scene");
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const bool selected = (i == currentSceneIndex_);
        if (ImGui::Selectable(sceneRegistry_[i].name.c_str(), selected))
            pendingSceneIndex_ = i;
    }
    ImGui::End();

    ImGui::Begin("Lighting");
    ImGui::ColorEdit3("Ambient Color", &ambientColor_.x);
    ImGui::DragFloat("Ambient Intensity", &ambientIntensity_, 0.01f, 0.0f,
                     10.0f);
    const size_t sceneLightCount = currentScene_ ? currentScene_->lights().size()
                                                 : 0;
    ImGui::Text("Scene lights: %zu", sceneLightCount);
    ImGui::Text("Uploaded: %u directional, %u punctual",
                lastUploadedDirectionalLights_, lastUploadedPunctualLights_);
    if (lastIgnoredLights_ > 0)
        ImGui::Text("Ignored: %u", lastIgnoredLights_);
    if (sceneLightCount == 0) {
        ImGui::Separator();
        ImGui::DragFloat3("Sun Direction", &defaultSunDirection_.x, 0.01f,
                          -1.0f, 1.0f);
        ImGui::ColorEdit3("Sun Color", &defaultSunColor_.x);
        ImGui::DragFloat("Sun Intensity", &defaultSunIntensity_, 0.05f, 0.0f,
                         20.0f);
    }
    ImGui::End();

    ImGui::Begin("Materials");
    if (!currentScene_) {
        ImGui::Text("Materials: 0");
    } else {
        const auto &materials = currentScene_->materials();
        ImGui::Text("Materials: %zu", materials.size());
        for (size_t i = 0; i < materials.size(); ++i) {
            const auto &material = materials[i];
            if (!material) {
                ImGui::Text("#%zu <null>", i);
                continue;
            }

            const auto &params = material->params();
            const std::string label =
                "#" + std::to_string(i) + " " +
                (params.debugName.empty() ? "<unnamed>" : params.debugName);
            if (!ImGui::TreeNode(label.c_str()))
                continue;

            ImGui::Text("Alpha Mode: %s", alphaModeName(params.alphaMode));
            ImGui::Text("Alpha Cutoff: %.3f", params.alphaCutoff);
            ImGui::Text("Double Sided: %s",
                        params.doubleSided ? "true" : "false");
            ImGui::Text("Transmission: %.3f", params.transmissionFactor);
            ImGui::Text("Emissive Strength: %.3f",
                        params.emissiveStrength);
            ImGui::Text("Metallic Factor: %.3f", params.metallicFactor);
            ImGui::Text("Roughness Factor: %.3f", params.roughnessFactor);
            ImGui::Text("Occlusion Strength: %.3f",
                        params.occlusionStrength);
            ImGui::Text("Occlusion UV: %u", params.occlusionTexCoord);
            ImGui::Text("Base Color Factor: %.3f %.3f %.3f %.3f",
                        params.baseColorFactor.r, params.baseColorFactor.g,
                        params.baseColorFactor.b, params.baseColorFactor.a);
            ImGui::Text("Emissive Factor: %.3f %.3f %.3f",
                        params.emissiveFactor.r, params.emissiveFactor.g,
                        params.emissiveFactor.b);
            ImGui::Separator();
            ImGui::Text("Render Queue: %s",
                        isTransparentMaterial(params) ? "Transparent"
                                                      : "Opaque");
            ImGui::Text("Cull: %s", params.doubleSided ? "None" : "Back");
            ImGui::Separator();
            const auto &textures = material->textures();
            for (size_t slotIndex = 0; slotIndex < kMaterialTextureSlotCount;
                 ++slotIndex) {
                const auto slot =
                    static_cast<MaterialTextureSlot>(slotIndex);
                ImGui::Text("%s: %s", slotName(slot),
                            textures[slotIndex] ? "Bound" : "Missing");
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::Begin("Stats");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    const auto p = camera_.position();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Mode:   %s", mode_ == InputMode::UI ? "UI" : "CameraDrag");
    if (currentScene_)
        ImGui::Text("Objects: %zu", currentScene_->objects().size());
    ImGui::Text("(Hold RMB in viewport to fly, WASD/Q/E to move)");
    ImGui::End();
}

void Application::handleSwapChainRecreate() {
    renderer_->recreateSwapChain();
    pipelineCache_->clear();
    frameSync_->onSwapChainRecreated();
    gui_->onSwapChainRecreated(swapChain_->imageCount());
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
}

const ShaderVariant &Application::currentShaderVariant() const {
    if (shaderVariants_.empty())
        return defaultShaderVariant();
    const int index = std::clamp(currentShaderVariantIndex_, 0,
                                 static_cast<int>(shaderVariants_.size()) - 1);
    return shaderVariants_[index];
}

void Application::mainLoop() {
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime = startTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();
        input_->update();

        // 1. 帧外：场景切�?
        if (pendingSceneIndex_ != -1) {
            switchScene(pendingSceneIndex_);
            pendingSceneIndex_ = -1;
        }

        // 2. 时间
        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // 3. ImGui 新帧
        gui_->beginFrame();

        // 4. 模式切换 + 输入
        updateInputMode();
        if (mode_ == InputMode::CameraDrag)
            processCameraInput(dt);
        if (input_->isKeyDown(Key::Escape))
            window_->setShouldClose(true);

        // 5. 场景 tick
        float t = std::chrono::duration<float>(now - startTime).count();
        if (currentScene_)
            currentScene_->update(dt, t);

        // 6. UI
        drawGui();

        // 7. 渲染
        auto ctx = frameSync_->beginFrame();
        if (!ctx) {
            if (frameSync_->swapChainNeedsRecreation())
                handleSwapChainRecreate();
            ImGui::EndFrame();
            input_->endFrame();
            continue;
        }

        updateUniforms(ctx->frameIndex);
        renderQueue_.clear();
        if (currentScene_)
            currentScene_->collectRenderCommands(renderQueue_);
        renderQueue_.sortOpaque();
        renderQueue_.sortTransparent(camera_.position());

        renderer_->renderFrame(*ctx, renderQueue_, *pipelineCache_, *gui_,
                               currentShaderVariant());
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation())
            handleSwapChainRecreate();

        // 8. 帧末：丢弃本帧鼠标增量
        input_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
