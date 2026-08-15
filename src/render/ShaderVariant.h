#pragma once

#include <cstdint>
#include <string>
#include "render/MaterialBindingMode.h"

namespace vkr {

enum class ShaderProgramContract {
    MainForward,
    ShadowDepth,
    PunctualShadowDepth,
    SurfacePrepass,
    Fullscreen,
    Compute,
};

enum class ShaderToneMappingPolicy {
    PassThrough,
    Configurable,
};

struct ShaderProgram {
    std::string id;
    ShaderProgramContract contract = ShaderProgramContract::MainForward;
    std::string vertexSourcePath;
    std::string fragmentSourcePath;
    std::string computeSourcePath;
    std::string vertSpvPath;
    std::string fragSpvPath;
    std::string bindlessFragSpvPath;
    std::string computeSpvPath;
    bool usesSceneLights = false;
    bool usesAtmosphere = false;
    bool usesScreenSpace = false;
    bool usesDdgi = false;
    bool usesMaterialTextures = false;

    const std::string &fragmentSpvPath(MaterialBindingMode mode) const {
        return mode == MaterialBindingMode::Bindless &&
                       !bindlessFragSpvPath.empty()
                   ? bindlessFragSpvPath
                   : fragSpvPath;
    }
};

struct ShaderVariant {
    std::string id;
    std::string displayName;
    std::string programId;
    std::string category;
    int32_t order = 0;
    bool isDefault = false;
    bool supportsBloom = false;
    bool supportsAtmosphere = false;
    bool supportsScreenSpace = false;
    bool supportsDdgi = false;
    ShaderToneMappingPolicy toneMapping =
        ShaderToneMappingPolicy::PassThrough;
    std::string vertSpvPath;
    std::string fragSpvPath;
    std::string bindlessFragSpvPath;

    const std::string &fragmentSpvPath(MaterialBindingMode mode) const {
        return mode == MaterialBindingMode::Bindless &&
                       !bindlessFragSpvPath.empty()
                   ? bindlessFragSpvPath
                   : fragSpvPath;
    }
};

} // namespace vkr
