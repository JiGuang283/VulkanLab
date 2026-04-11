#include "app.h"

#include <chrono>

// ---- 应用入口 ----

void HelloTriangleApplication::run() {
    window_ = std::make_unique<vkr::Window>(WIDTH, HEIGHT, "Vulkan");
    input_ = std::make_unique<vkr::InputManager>(*window_);

    initVulkan();

    window_->setResizeCallback([this](int, int) { renderer_->notifyResize(); });

    mainLoop();
    cleanup();
}

// ---- Vulkan 初始化 ----

void HelloTriangleApplication::initVulkan() {
    context = std::make_unique<vkr::VulkanContext>(window_->handle());
    device = std::make_unique<vkr::Device>(*context);
    swapChain_ = std::make_unique<vkr::SwapChain>(*device, context->surface(),
                                                  window_->handle());
    renderer_ = std::make_unique<vkr::Renderer>(*device, *swapChain_,
                                                sizeof(UniformBufferObject));
    texture_ =
        std::make_shared<vkr::Texture>(*device, *renderer_, TEXTURE_PATH);
    material_ = std::make_shared<vkr::Material>(
        *device, *renderer_, *texture_, "shader/vert.spv", "shader/frag.spv");
    mesh_ = vkr::Mesh::fromOBJ(*device, *renderer_, MODEL_PATH);

    scene_.addObject({mesh_, material_, glm::mat4(1.0f)});

    camera_.setAspect(swapChain_->extent().width /
                      (float)swapChain_->extent().height);
}

// ---- 主循环 ----

void HelloTriangleApplication::mainLoop() {
    input_->setCursorCaptured(true);

    auto lastTime = std::chrono::high_resolution_clock::now();
    auto startTime = lastTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();

        // ---- deltaTime ----
        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // ---- 输入处理 ----
        input_->update();

        if (input_->isKeyDown(GLFW_KEY_ESCAPE))
            window_->setShouldClose(true);

        // WASD 移动
        const float speed = 2.0f;
        glm::vec3   move{0.0f};
        if (input_->isKeyDown(GLFW_KEY_W))
            move.z += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_S))
            move.z -= speed * dt;
        if (input_->isKeyDown(GLFW_KEY_A))
            move.x -= speed * dt;
        if (input_->isKeyDown(GLFW_KEY_D))
            move.x += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_SPACE))
            move.y += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_LEFT_SHIFT))
            move.y -= speed * dt;
        camera_.translate(move);

        // 鼠标旋转
        const float sensitivity = 0.1f;
        auto        delta = input_->mouseDelta();
        camera_.rotate(delta.x * sensitivity, -delta.y * sensitivity);

        // ---- 渲染 ----
        VkCommandBuffer cmd = renderer_->beginFrame();
        if (!cmd)
            continue;

        updateUniformBuffer(renderer_->frameIndex());

        // 更新场景物体变换
        float time = std::chrono::duration<float>(now - startTime).count();
        scene_.objects()[0].transform =
            glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f));

        renderer_->beginRenderPass(cmd);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChain_->extent().width);
        viewport.height = static_cast<float>(swapChain_->extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChain_->extent();
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        scene_.render(cmd, renderer_->frameIndex());

        renderer_->endRenderPass(cmd);
        renderer_->endFrame();
    }

    vkDeviceWaitIdle(device->logicalDevice());
}

// ---- 资源清理 ----

void HelloTriangleApplication::cleanup() {
    renderer_.reset();

    material_.reset();
    texture_.reset();
    mesh_.reset();

    swapChain_.reset();
    device.reset();
    context.reset();

    input_.reset();
    window_.reset();
}

// ---- Uniform 更新 ----

void HelloTriangleApplication::updateUniformBuffer(uint32_t currentImage) {
    UniformBufferObject ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();

    memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
}
