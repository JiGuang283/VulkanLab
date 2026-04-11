#pragma once

#include "vulkan_utils.h"

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Renderer.h"
#include "render/Texture.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "window/InputManager.h"
#include "window/Window.h"

class HelloTriangleApplication {
  public:
    void run();

  private:
    // ---- 窗口 / 输入 ----
    std::unique_ptr<vkr::Window>       window_;
    std::unique_ptr<vkr::InputManager> input_;

    // ---- Vulkan 核心对象 ----
    std::unique_ptr<vkr::VulkanContext> context;

    // ---- 设备 ----
    std::unique_ptr<vkr::Device> device;

    // ---- 交换链 ----
    std::unique_ptr<vkr::SwapChain> swapChain_;

    // ---- Renderer ----
    std::unique_ptr<vkr::Renderer> renderer_;

    // ---- 资源 ----
    std::shared_ptr<vkr::Texture>  texture_;
    std::shared_ptr<vkr::Material> material_;
    std::shared_ptr<vkr::Mesh>     mesh_;

    // ---- 场景 ----
    vkr::Scene  scene_;
    vkr::Camera camera_;

    // ---- 应用主框架 (app.cpp) ----
    void initVulkan();
    void mainLoop();
    void cleanup();

    void updateUniformBuffer(uint32_t currentImage);
};
