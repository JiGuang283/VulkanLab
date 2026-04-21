#include "Application.h"
#include "UniformData.h"

#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Pipeline.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "render/GuiSystem.h"
#include "render/Material.h"
#include "render/Renderer.h"
#include "scene/SceneFactory.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

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
    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions));
    device_ = std::make_unique<Device>(*context_);
    swapChain_ =
        std::make_unique<SwapChain>(*device_, context_->surface(), [this]() {
            return window_->framebufferExtent();
        });
    frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
    renderer_ = std::make_unique<Renderer>(*device_, *swapChain_, *frameSync_,
                                           sizeof(GlobalUBO));

    window_->setResizeCallback(
        [this](int, int) { frameSync_->notifyResize(); });

    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));

    if (sceneRegistry_.empty())
        throw std::runtime_error("No scenes registered; call "
                                 "Application::registerScene before run().");

    // Bootstrap: build the first scene so we can harvest its Material's
    // descriptor set layout, then create the shared opaque pipeline.
    const int start = std::clamp(config_.defaultSceneIndex, 0,
                                 static_cast<int>(sceneRegistry_.size()) - 1);
    currentScene_ =
        sceneRegistry_[start].factory(*device_, *frameSync_, *renderer_);
    currentSceneIndex_ = start;

    if (currentScene_->objects().empty())
        throw std::runtime_error("Bootstrap scene has no objects.");
    const auto &firstMat = *currentScene_->objects().front().material;
    opaquePipeline_ = std::make_unique<Pipeline>(
        *device_, renderer_->renderPass(), firstMat.pipelineConfig());

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
    currentScene_.reset();

    const auto &entry = sceneRegistry_[index];
    currentScene_ = entry.factory(*device_, *frameSync_, *renderer_);
    currentSceneIndex_ = index;

    if (currentScene_->initialCamera) {
        const auto &p = *currentScene_->initialCamera;
        camera_.setPosition(p.position);
        camera_.setYawPitch(p.yaw, p.pitch);
    }

    std::cout << "[Scene] switched to " << entry.name << std::endl;
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
    if (input_->isKeyDown(Key::Space))
        move.y += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::LeftShift))
        move.y -= config_.moveSpeed * dt;
    camera_.translate(move);

    const auto d = input_->mouseDelta();
    camera_.rotate(d.x * config_.mouseSensitivity,
                   -d.y * config_.mouseSensitivity);
}

void Application::updateUniforms(uint32_t frameIndex) {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    std::memcpy(renderer_->mappedUniformBuffer(frameIndex), &ubo, sizeof(ubo));
}

void Application::drawGui() {
    ImGui::Begin("Scene");
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const bool selected = (i == currentSceneIndex_);
        if (ImGui::Selectable(sceneRegistry_[i].name.c_str(), selected))
            pendingSceneIndex_ = i;
    }
    ImGui::End();

    ImGui::Begin("Stats");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    const auto p = camera_.position();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Mode:   %s", mode_ == InputMode::UI ? "UI" : "CameraDrag");
    if (currentScene_)
        ImGui::Text("Objects: %zu", currentScene_->objects().size());
    ImGui::Text("(Hold RMB in viewport to fly, WASD/Space/Shift to move)");
    ImGui::End();
}

void Application::handleSwapChainRecreate() {
    renderer_->recreateSwapChain();
    frameSync_->onSwapChainRecreated();
    gui_->onSwapChainRecreated(swapChain_->imageCount());
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
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
            continue;
        }

        updateUniforms(ctx->frameIndex);

        renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
        if (currentScene_)
            currentScene_->render(ctx->cmd, ctx->frameIndex, *opaquePipeline_);
        gui_->render(ctx->cmd);
        renderer_->endRenderPass(ctx->cmd);
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation())
            handleSwapChainRecreate();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
