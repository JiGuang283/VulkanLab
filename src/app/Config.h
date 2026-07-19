#pragma once

#include <cstdint>
#include <string>

namespace vkr {

enum class AssetImportMode { OnDemand, ReadOnly, CookedOnly };

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
    std::string assetToolPath;
    bool cachePathExplicit = false;
    bool assetToolPathExplicit = false;

    // ---- 渲染设置 ----
    bool enableValidation = true;
    bool enableRuntimeControl = false;
    uint32_t gltfMaxTextureSize = 2048; // 0 = Full resolution
    AssetImportMode assetImportMode = AssetImportMode::OnDemand;
    bool assetImportModeExplicit = false;
    uint32_t assetImportWorkers = 0;
    uint64_t assetImportMemoryBudgetMiB = 2048;

    // ---- 输入参数 ----
    float moveSpeed = 2.0f;
    float mouseSensitivity = 0.1f;

    // ---- 场景 ----
    int defaultSceneIndex = 0;
};

} // namespace vkr
