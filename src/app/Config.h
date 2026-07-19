#pragma once

#include <cstdint>
#include <string>

namespace vkr {

struct Config {
    // ---- 窗口 ----
    uint32_t    windowWidth = 800;
    uint32_t    windowHeight = 600;
    std::string windowTitle = "Vulkan Renderer";

    // ---- 资源路径 ----
    std::string texturePath = "textures/viking_room.png";
    std::string vertShaderPath = "shader/legacy/forward.vert.spv";
    std::string fragShaderPath = "shader/legacy/forward.frag.spv";
    std::string derivedTextureCachePath;
    std::string projectPath;

    // ---- 渲染设置 ----
    bool enableValidation = true;
    bool enableRuntimeControl = false;
    uint32_t gltfMaxTextureSize = 2048; // 0 = Full resolution

    // ---- 输入参数 ----
    float moveSpeed = 2.0f;
    float mouseSensitivity = 0.1f;

    // ---- 场景 ----
    int defaultSceneIndex = 0;
};

} // namespace vkr
