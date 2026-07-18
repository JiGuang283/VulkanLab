# 第六步：ImGui 集成

## 目标

集成 Dear ImGui（Vulkan + GLFW 后端），在主 RenderPass 中以最后一个 subpass 的方式绘制 UI 覆盖层。初始功能：显示 FPS、相机位置、场景对象列表。

## 前置条件

- step2 完成（FrameSync 或等效帧同步已稳定）
- 现有渲染流程可正常工作

## 方案选择

| 方案 | 优点 | 缺点 |
|------|------|------|
| 独立 RenderPass | ImGui 完全解耦 | 多一次 RenderPass 切换开销 |
| 同 RenderPass 最后绘制 | 零额外 RenderPass | RenderPass 配置略复杂 |
| subpass | 理论最优 | 桌面端收益极小，复杂度高 |

**选择：同 RenderPass、在 endRenderPass 前绘制 ImGui**。最简单，桌面端开销可忽略。

## 改动清单

### A. 引入 ImGui 源码

```
external/
  imgui/
    imconfig.h
    imgui.h
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui_demo.cpp          ← 调试用，可选
    backends/
      imgui_impl_glfw.h
      imgui_impl_glfw.cpp
      imgui_impl_vulkan.h
      imgui_impl_vulkan.cpp
```

从 [Dear ImGui releases](https://github.com/ocornut/imgui) 取最新 docking 分支或 stable。

CMakeLists.txt 增加 ImGui 源文件和 include：
```cmake
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

add_executable(VulkanLab ${SOURCES} ${IMGUI_SOURCES})

target_include_directories(VulkanLab PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
```

### B. 新建 `src/render/GuiSystem.h / .cpp`

```cpp
// GuiSystem.h
#pragma once

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace vkr {

class Device;

class GuiSystem {
  public:
    /// @param minImageCount  swap chain 最小 image 数
    /// @param imageCount     swap chain 实际 image 数
    GuiSystem(Device &device, VkRenderPass renderPass,
              GLFWwindow *window, uint32_t minImageCount, uint32_t imageCount);
    ~GuiSystem();

    // 不可复制/移动
    GuiSystem(const GuiSystem &) = delete;
    GuiSystem &operator=(const GuiSystem &) = delete;

    /// 开始新一帧 ImGui（在输入处理之后、渲染之前调用）
    void beginFrame();

    /// 结束 ImGui 帧并录制绘制命令到 cmd
    void render(VkCommandBuffer cmd);

    /// swap chain 重建时调用
    void onSwapChainRecreated(uint32_t minImageCount, uint32_t imageCount);

    /// 查询 ImGui 是否想捕获鼠标/键盘（用于屏蔽游戏输入）
    bool wantCaptureMouse() const;
    bool wantCaptureKeyboard() const;

  private:
    void createDescriptorPool();

    Device        *device_;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
};

} // namespace vkr
```

```cpp
// GuiSystem.cpp
#include "GuiSystem.h"
#include "core/Device.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace vkr {

GuiSystem::GuiSystem(Device &device, VkRenderPass renderPass,
                     GLFWwindow *window, uint32_t minImageCount,
                     uint32_t imageCount)
    : device_(&device)
{
    createDescriptorPool();

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window, true);  // true = install callbacks

    // Vulkan backend
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance       = device.instance();
    initInfo.PhysicalDevice = device.physicalDevice();
    initInfo.Device         = device.handle();
    initInfo.QueueFamily    = device.graphicsQueueFamily();
    initInfo.Queue          = device.graphicsQueue();
    initInfo.DescriptorPool = descriptorPool_;
    initInfo.MinImageCount  = minImageCount;
    initInfo.ImageCount     = imageCount;
    initInfo.MSAASamples    = device.msaaSamples();
    initInfo.RenderPass     = renderPass;
    initInfo.Subpass        = 0;

    ImGui_ImplVulkan_Init(&initInfo);

    // 上传字体纹理（ImGui 1.91+ 在 Init 内自动上传，旧版需手动）
    // 若使用旧版本：
    // ImGui_ImplVulkan_CreateFontsTexture();
}

GuiSystem::~GuiSystem() {
    device_->waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device_->handle(), descriptorPool_, nullptr);
}

void GuiSystem::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void GuiSystem::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void GuiSystem::onSwapChainRecreated(uint32_t minImageCount,
                                      uint32_t imageCount)
{
    ImGui_ImplVulkan_SetMinImageCount(minImageCount);
    // imageCount 若变化可在此处理
}

bool GuiSystem::wantCaptureMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool GuiSystem::wantCaptureKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void GuiSystem::createDescriptorPool() {
    // ImGui 需要的 descriptor 类型
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 100;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = poolSizes;

    vkCreateDescriptorPool(device_->handle(), &poolInfo, nullptr, &descriptorPool_);
}

} // namespace vkr
```

### C. Application 集成

```cpp
// Application.h
#include "render/GuiSystem.h"

class Application {
    // ...
    std::unique_ptr<GuiSystem> gui_;
};

// Application.cpp — init() 末尾
gui_ = std::make_unique<GuiSystem>(
    *device_, renderer_->renderPass(),
    window_->handle(),
    swapChain_->minImageCount(), swapChain_->imageCount());

// Application.cpp — mainLoop
void Application::mainLoop() {
    while (!window_->shouldClose()) {
        window_->pollEvents();

        // ImGui 新帧（在输入处理之前，让 ImGui 能截获输入）
        gui_->beginFrame();

        // 只在 ImGui 不捕获输入时处理游戏输入
        if (!gui_->wantCaptureMouse() && !gui_->wantCaptureKeyboard()) {
            inputManager_->processInput(camera_);
        }

        auto frameCtx = renderer_->beginFrame();
        if (!frameCtx) continue;

        updateUniforms(frameCtx->frameIndex);
        renderer_->beginRenderPass(frameCtx->cmd);

        scene_.render(frameCtx->cmd, frameCtx->frameIndex /*, ... */);

        // ---- ImGui 绘制（在 RenderPass 结束前）----
        drawGui();          // 用户 UI 逻辑
        gui_->render(frameCtx->cmd);

        renderer_->endRenderPass(frameCtx->cmd);
        renderer_->endFrame();
    }
}

// Application.cpp — UI 内容
void Application::drawGui() {
    ImGui::Begin("Info");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Camera: (%.1f, %.1f, %.1f)",
                camera_.position().x, camera_.position().y, camera_.position().z);
    ImGui::Text("Objects: %zu", scene_.objectCount());
    ImGui::End();
}

// Application.cpp — recreateSwapChain
gui_->onSwapChainRecreated(swapChain_->minImageCount(), swapChain_->imageCount());
```

### D. InputManager 适配

ImGui_ImplGlfw 通过 `glfwSetXxxCallback()` 安装自己的回调。如果 InputManager 也用了 GLFW 回调，有两种处理：

1. **让 ImGui 先装回调（推荐）**：`ImGui_ImplGlfw_InitForVulkan(window, true)` 会 chain 原有回调。只需确保 InputManager 在 ImGui Init 之前设置回调，ImGui 会自动转发。
2. **polling 模式不受影响**：如果 InputManager 使用 `glfwGetKey()` / `glfwGetCursorPos()`（polling），则完全不冲突。

当前 InputManager 使用 polling（`glfwGetKey`），不需要改动。

### E. 鼠标捕获问题

当前若使用了 `glfwSetInputMode(GLFW_CURSOR_DISABLED)` 来做 FPS 相机，需要在 ImGui 活跃时释放鼠标：

```cpp
// 在 mainLoop 中，ImGui beginFrame 之后
if (gui_->wantCaptureMouse()) {
    glfwSetInputMode(window_->handle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
} else {
    glfwSetInputMode(window_->handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}
```

或者用一个 toggle 键（如 Tab/Escape）来切换鼠标捕获状态。

## 验证

1. **编译通过** —— ImGui 源码正确编译
2. **窗口右上角出现 "Info" 面板**，显示 FPS、相机位置、对象数
3. **鼠标点击 ImGui 面板时**，相机不响应移动
4. **拉伸窗口** → ImGui 正确适配新分辨率
5. **ImGui Demo**：可选临时加 `ImGui::ShowDemoWindow()` 全面测试

## 后续扩展点

- 材质参数调节面板（float slider 控制 roughness / metallic）
- 场景 hierarchy 面板
- 光源编辑器
- Profiler 面板（draw call 数、GPU 时间）

## 文件变更总结

```
新增：external/imgui/              (ImGui 源码)
新增：src/render/GuiSystem.h
新增：src/render/GuiSystem.cpp
修改：CMakeLists.txt               (ImGui 源文件 + include)
修改：src/app/Application.h        (持有 GuiSystem)
修改：src/app/Application.cpp      (init/mainLoop/drawGui/recreate)
```
