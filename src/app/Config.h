#pragma once

#include <BuildFeatures.h>

#include "assets/AssetImportMode.h"
#include "core/ValidationProfile.h"
#include "diagnostics/DiagnosticsConfig.h"
#include "core/MaterialBindingMode.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace vkr {

struct Config {
    // ---- 窗口 ----
    uint32_t    windowWidth = 800;
    uint32_t    windowHeight = 600;
    std::string windowTitle = "Vulkan Renderer";

    // ---- 资源路径 ----
    std::string derivedTextureCachePath;
    std::filesystem::path projectPath;
    std::string assetToolPath;
    std::string gltfValidatorPath;
    bool cachePathExplicit = false;
    bool assetToolPathExplicit = false;
    bool gltfValidatorPathExplicit = false;

    // ---- 渲染设置 ----
    ValidationProfile validationProfile =
        build::kValidation ? ValidationProfile::Core
                           : ValidationProfile::Off;
    bool enableRuntimeControl = false;
    uint32_t gltfMaxTextureSize = 2048; // 0 = Full resolution
    MaterialBindingMode materialBindingMode = MaterialBindingMode::Auto;
    AssetImportMode assetImportMode =
        build::kAssetAuthoring ? AssetImportMode::OnDemand
                               : AssetImportMode::ReadOnly;
    bool assetImportModeExplicit = false;
    uint32_t assetImportWorkers = 0;
    uint64_t assetImportMemoryBudgetMiB = 2048;
    DiagnosticsConfig diagnostics;

    // ---- 输入参数 ----
    float moveSpeed = 2.0f;
    float mouseSensitivity = 0.1f;

    // ---- 场景 ----
    int defaultSceneIndex = 0;
};

} // namespace vkr
