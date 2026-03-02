## Plan: Vulkan 项目中度模块化重构

将 `HelloTriangleApplication` 这个 God Class 拆解为多个职责单一的 RAII 类，同时现代化 CMake 构建系统。重构后每个 Vulkan 子系统（Instance、Device、Swapchain、Pipeline、RenderPass、Renderer）各有独立的 `.h/.cpp`，通过组合关系在顶层 `Application` 类中协调。不改变渲染功能（仍为硬编码三角形）。

**Steps**

### 1. 重新组织目录结构

创建如下目录布局：

```
src/
├── main.cpp
├── application.h / application.cpp          ← 顶层应用（窗口 + 主循环）
├── core/
│   ├── vulkan_instance.h / .cpp             ← VkInstance + DebugMessenger
│   ├── vulkan_device.h / .cpp               ← PhysicalDevice + LogicalDevice + Queues
│   ├── vulkan_swapchain.h / .cpp            ← Swapchain + ImageViews + recreate 逻辑
│   ├── vulkan_renderpass.h / .cpp           ← RenderPass
│   ├── vulkan_pipeline.h / .cpp             ← PipelineLayout + Pipeline + ShaderModule
│   ├── vulkan_framebuffers.h / .cpp         ← Framebuffers
│   └── vulkan_command.h / .cpp              ← CommandPool + CommandBuffers
├── renderer/
│   └── renderer.h / .cpp                    ← 帧绘制 + 同步对象 + drawFrame 逻辑
└── utils/
    ├── vulkan_types.h                       ← 常量、QueueFamilyIndices、SwapChainSupportDetails 等共享类型
    └── file_utils.h / .cpp                  ← readFile() 等工具函数
```

### 2. 提取共享类型 — `utils/vulkan_types.h`

将 `src/vulkan_utils.h` 中的内容拆分：
- `QueueFamilyIndices`、`SwapChainSupportDetails` 结构体 → `vulkan_types.h`
- `MAX_FRAMES_IN_FLIGHT` → `vulkan_types.h`，改为 `constexpr`
- `validationLayers`、`deviceExtensions` → `vulkan_types.h`，改为 `constexpr std::array`
- `enableValidationLayers` → 改为基于 `NDEBUG` 宏的 `constexpr bool`：`#ifdef NDEBUG constexpr bool enableValidationLayers = false; #else ... = true; #endif`
- `WIDTH`、`HEIGHT` → 移入 `Application` 类或作为构造参数
- `CreateDebugUtilsMessengerEXT` / `DestroyDebugUtilsMessengerEXT` → 移入 `VulkanInstance` 实现内部（不暴露）

### 3. 创建 `VulkanInstance` 类 — `core/vulkan_instance.h/.cpp`

从 `src/vulkan_instance.cpp` 提取成独立 RAII 类：
- **构造函数**：执行 `createInstance()` + `setupDebugMessenger()`
- **析构函数**：销毁 `VkDebugUtilsMessengerEXT` 和 `VkInstance`
- **公开接口**：`VkInstance getInstance() const`；`operator VkInstance() const` 可选
- **私有方法**：`checkValidationLayerSupport()`、`getRequiredExtensions()`、`populateDebugMessengerCreateInfo()`
- `debugCallback` 保持为 `static` 成员函数
- 禁止拷贝（`= delete`），支持移动

### 4. 创建 `VulkanDevice` 类 — `core/vulkan_device.h/.cpp`

从 `src/vulkan_device.cpp` 提取：
- **构造参数**：`VkInstance`、`VkSurfaceKHR`
- **构造函数**：执行 `pickPhysicalDevice()` + `createLogicalDevice()`
- **析构函数**：`vkDestroyDevice()`
- **公开接口**：`VkDevice device()`、`VkPhysicalDevice physicalDevice()`、`VkQueue graphicsQueue()`、`VkQueue presentQueue()`、`findQueueFamilies()`、`querySwapChainSupport()`
- `querySwapChainSupport()` 和 `findQueueFamilies()` 需要对外暴露，因为 Swapchain 创建时需要用

### 5. 创建 `VulkanSwapchain` 类 — `core/vulkan_swapchain.h/.cpp`

从 `src/vulkan_swapchain.cpp` 提取：
- **构造参数**：`VulkanDevice&`、`VkSurfaceKHR`、`GLFWwindow*`、可选 `VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE`
- **析构函数**：销毁 `ImageViews` + `VkSwapchainKHR`
- **公开接口**：`VkSwapchainKHR handle()`、`VkFormat imageFormat()`、`VkExtent2D extent()`、`const std::vector<VkImageView>& imageViews()`、`size_t imageCount()`
- `recreate()` 方法：内部销毁旧资源后重新创建
- `createSurface()` 可以作为 **自由函数** 或 `Application` 的职责（因为它依赖 `GLFWwindow` 和 `VkInstance`，生命周期独立于 Swapchain）

### 6. 创建 `VulkanRenderPass` 类 — `core/vulkan_renderpass.h/.cpp`

从 `src/vulkan_renderpass.cpp` 提取：
- **构造参数**：`VkDevice`、`VkFormat swapchainImageFormat`
- **析构函数**：`vkDestroyRenderPass()`
- **公开接口**：`VkRenderPass handle()`

### 7. 创建 `VulkanPipeline` 类 — `core/vulkan_pipeline.h/.cpp`

从 `src/vulkan_pipeline.cpp` 提取：
- **构造参数**：`VkDevice`、`VkRenderPass`、`VkExtent2D`（或结构体 `PipelineConfig`）
- **析构函数**：销毁 `VkPipeline` + `VkPipelineLayout`
- **私有方法**：`createShaderModule()` — **修复参数为 `const std::vector<char>&`**（当前是按值传递的 bug）
- **公开接口**：`VkPipeline handle()`、`VkPipelineLayout layout()`
- `readFile()` 移入 `utils/file_utils.h`

### 8. 创建 `VulkanFramebuffers` 类 — `core/vulkan_framebuffers.h/.cpp`

从 `src/vulkan_framebuffers.cpp` 提取：
- **构造参数**：`VkDevice`、`VkRenderPass`、`VkExtent2D`、`const std::vector<VkImageView>&`
- **析构函数**：销毁所有 `VkFramebuffer`
- **公开接口**：`VkFramebuffer operator[](size_t index)`、`size_t count()`

### 9. 创建 `VulkanCommand` 类 — `core/vulkan_command.h/.cpp`

从 `src/vulkan_commandpool.cpp` 提取：
- **构造参数**：`VkDevice`、`uint32_t queueFamilyIndex`
- **析构函数**：`vkDestroyCommandPool()`（CommandBuffer 会随 Pool 一起回收）
- **公开接口**：`VkCommandPool pool()`、`allocateBuffers(uint32_t count) → std::vector<VkCommandBuffer>`
- `recordCommandBuffer()` → 移入 `Renderer`（因为它包含 renderpass/pipeline/framebuffer 逻辑）

### 10. 创建 `Renderer` 类 — `renderer/renderer.h/.cpp`

从 `src/vulkan_drawframe.cpp` + 部分 `app.cpp` 提取：
- **职责**：管理同步对象、录制命令、提交绘制
- **构造参数**：`VulkanDevice&`、`VulkanSwapchain&`、`VulkanRenderPass&`、`VulkanPipeline&`、`VulkanFramebuffers&`、`VulkanCommand&`
- **析构函数**：销毁所有 `VkSemaphore` 和 `VkFence`
- **公开接口**：`drawFrame()`、`waitIdle()`
- `recordCommandBuffer()` 作为私有方法移入此类
- **修复 bug**：删除 `vulkan_drawframe.cpp` 中重复调用的 `vkAcquireNextImageKHR()`

### 11. 重写 `Application` 类 — `application.h/.cpp`

从 `src/app.h` / `src/app.cpp` 精简为顶层协调器：
- **成员**：`GLFWwindow*`、`VkSurfaceKHR`，以及上述各模块类的实例（按创建顺序作为成员，析构时会以 **逆序** 自动 RAII 销毁）
- **构造函数**：初始化 GLFW 窗口
- **方法**：`run()`（主循环）、`initVulkan()`（按顺序构造各模块）、用 `std::unique_ptr` 管理可重建的模块（Swapchain/Framebuffers）以支持 recreate
- `VkSurfaceKHR` 的创建和销毁留在 `Application` 中（因为它连接 Window 和 Instance）
- `framebufferResizeCallback` 保持为静态函数

### 12. 现代化 CMake 构建系统

改造 `CMakeLists.txt`：
- 使用 `target_compile_options` 替代全局 `add_compile_options`
- 使用 `target_include_directories` 设置 `src/` 为 include 根目录，使得 `#include "core/vulkan_device.h"` 成为可能
- GLFW 改用 `target_link_directories` + `target_link_libraries`，或更好的做法是创建一个 IMPORTED target
- 添加 `cmake_dependent_option` 或直接通过 `CMAKE_BUILD_TYPE` / generator expression 控制 `NDEBUG` 宏，使 Release 构建自动关闭验证层
- Shader 编译：添加 `find_program(GLSLC glslc)` 自动查找 glslc，通过 `add_custom_command` 编译 `.vert`/`.frag` → `.spv`（替代硬编码路径的 `compile.bat`）
- 将源文件按子目录组织，在 `target_sources` 中列出

### 13. 修复已知 Bug

在重构过程中顺带修复：
- **严重**：`vulkan_drawframe.cpp` 中 `vkAcquireNextImageKHR()` 重复调用 → 删除第二次调用
- **中等**：`createShaderModule()` 参数改为 `const std::vector<char>&`
- **低**：`GLFW_RESIZABLE` 使用 `GLFW_FALSE` 替代 `GL_FALSE`
- **低**：fragment shader 中 `# version` 改为 `#version`（去掉空格）

**Verification**

1. 编译验证：`cmake --build build --config Debug` 和 `Release` 都应通过
2. 运行验证：程序应能正常显示彩色三角形
3. 验证层：Debug 构建应启用验证层且无错误输出；Release 构建应不加载验证层
4. 窗口缩放：如果启用 resize，recreateSwapchain 流程正常工作
5. 析构顺序：关闭窗口后无验证层报错（RAII 析构顺序与创建顺序相反）
6. Shader 编译：修改 `.vert`/`.frag` 后 CMake 重新构建应自动生成 `.spv`

**Decisions**

- `VkSurfaceKHR` 由 `Application` 管理而非 `VulkanSwapchain`，因为它的生命周期独立于 Swapchain 重建
- 使用 `std::unique_ptr` 管理可重建模块（Swapchain、Framebuffers），非 RAII 成员，以支持 `recreateSwapChain()` 中的销毁与重建
- 成员声明顺序即为创建顺序，C++ 析构顺序保证自动逆序销毁
- `recordCommandBuffer()` 归入 `Renderer` 而非 `VulkanCommand`，因为它对 Pipeline/RenderPass/Framebuffer 都有依赖
