#include "Application.h"
#include "UniformData.h"

#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "render/GltfLoader.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Renderer.h"
#include "render/Texture.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <chrono>
#include <cstring>

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

    texture_ =
        std::make_shared<Texture>(*device_, *frameSync_, config_.texturePath);
    material_ = std::make_shared<Material>(*device_, *renderer_, *texture_,
                                           config_.vertShaderPath,
                                           config_.fragShaderPath);
    // Branch on file extension: .glb/.gltf -> GltfLoader, otherwise OBJ
    const std::string &modelPath = config_.modelPath;
    const bool         isGltf =
        (modelPath.size() >= 4 &&
         (modelPath.compare(modelPath.size() - 4, 4, ".glb") == 0 ||
          modelPath.compare(modelPath.size() - 5, 5, ".gltf") == 0));

    if (isGltf) {
        auto rawMeshes = GltfLoader::load(modelPath, *device_, *frameSync_);
        for (auto &rm : rawMeshes) {
            auto sptr = std::shared_ptr<Mesh>(std::move(rm));
            gltfMeshes_.push_back(sptr);
            scene_.addObject({sptr, material_, glm::mat4(1.0f)});
        }
    } else {
        mesh_ = Mesh::fromOBJ(*device_, *frameSync_, modelPath);
        scene_.addObject({mesh_, material_, glm::mat4(1.0f)});
    }
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
}

void Application::processInput(float dt) {
    input_->update();

    if (input_->isKeyDown(Key::Escape))
        window_->setShouldClose(true);

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

    auto delta = input_->mouseDelta();
    camera_.rotate(delta.x * config_.mouseSensitivity,
                   -delta.y * config_.mouseSensitivity);
}

void Application::updateUniforms(uint32_t frameIndex) {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    std::memcpy(renderer_->mappedUniformBuffer(frameIndex), &ubo, sizeof(ubo));
}

void Application::mainLoop() {
    input_->setCursorCaptured(true);

    auto lastTime = std::chrono::high_resolution_clock::now();
    auto startTime = lastTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();

        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        processInput(dt);

        auto ctx = frameSync_->beginFrame();
        if (!ctx) {
            if (frameSync_->swapChainNeedsRecreation()) {
                renderer_->recreateSwapChain();
                frameSync_->onSwapChainRecreated();
                camera_.setAspect(
                    static_cast<float>(swapChain_->extent().width) /
                    static_cast<float>(swapChain_->extent().height));
            }
            continue;
        }

        updateUniforms(ctx->frameIndex);

        float time = std::chrono::duration<float>(now - startTime).count();
        scene_.objects()[0].transform =
            glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f));

        renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
        scene_.render(ctx->cmd, ctx->frameIndex);
        renderer_->endRenderPass(ctx->cmd);
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation()) {
            renderer_->recreateSwapChain();
            frameSync_->onSwapChainRecreated();
            camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                              static_cast<float>(swapChain_->extent().height));
        }
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
