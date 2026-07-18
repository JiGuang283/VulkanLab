# 构建与运行

> Status: Current
> Last verified: 2026-07-18
> Verified against: Stage 2 working tree based on `d4539b7`

## 环境要求

- Windows 10/11 和支持 Vulkan 的显卡驱动。
- Visual Studio 2022 C++ 工具链。
- CMake 3.20 或更高版本。
- Vulkan SDK。CMake 需要能找到 Vulkan、`glslc` 和 SDK 中的 GLM 头文件。
- 仓库内 `external/` 依赖完整，尤其是 `glfw/lib-vc2022`、ImGui、stb、VMA 和 glTF 头文件。

首次配置：

```powershell
cmake -S . -B build-debug
cmake -S . -B build
```

构建 Debug 和 Release：

```powershell
cmake --build build-debug --config Debug
cmake --build build --config Release
```

CMake 会在构建前编译 `shader/` 下登记的 Shader variant，并在构建后把 `shader/`、`textures/` 和 `models/` 复制到可执行文件目录。Windows 构建还会生成运行时控制工具。

```text
build-debug/Debug/VulkanLab.exe
build-debug/Debug/VulkanLabCtl.exe
build/Release/VulkanLab.exe
build/Release/VulkanLabCtl.exe
```

## 启动

程序使用相对路径加载资源，应把对应输出目录设为工作目录：

```powershell
cd build-debug\Debug
.\VulkanLab.exe
```

Runtime Control 默认关闭。需要从另一个终端控制运行中的渲染器时：

```powershell
.\VulkanLab.exe --runtime-control
```

查看启动参数不会初始化窗口或 Vulkan：

```powershell
.\VulkanLab.exe --help
```

未知参数会打印用法并返回非零退出码。控制工具的完整命令见 [Runtime Control](runtime_control.md)。

## 场景资产

`Viking Room` 和 `Sheen Chair` 会固定注册。以下 glTF 场景仅在对应文件存在时注册：

- `A Beautiful Game`
- `Anisotropy Barn Lamp`
- `Car Concept`
- `Chronograph Watch`
- `Diffuse Transmission Teacup`
- `Pot of Coals`
- `Main Sponza`

`Main Sponza` 的入口是 `models/main_sponza/NewSponza_Main_glTF_003.gltf`。缺失的可选模型只会被跳过，不影响程序启动。CMake 当前复制整个 `models/` 目录，大型本地资产会明显增加构建后的复制时间。

## 运行注意事项

- glTF 纹理尺寸默认限制为 `2048`，可在 Renderer 面板切换为 `Full`、`2048`、`1024` 或 `512`。切换会创建新的场景加载任务。
- glTF 解析、图片解码和 CPU 缩放在 worker 执行；GPU 创建和上传由主线程按帧推进。加载进度和取消操作位于 ImGui `Loading` 面板。
- GPU build 前会释放旧 Scene 以控制大场景切换时的显存峰值，因此该阶段可能只显示 ImGui 和空场景。
- `Full` 对 Main Sponza 仍是高风险选项；当前运行时使用 RGBA8，尚未接入 KTX2/BasisU 压缩资产。
- 日志写入运行目录下的 `logs/VulkanLab.log`。加载统计也显示在 ImGui 的 `Stats -> Last Scene Load`。
