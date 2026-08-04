#pragma once

#include <cstdint>
#include <string>

namespace vkr {

enum class ShaderProgramContract {
    MainForward,
    ShadowDepth,
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
    std::string computeSpvPath;
    bool usesSceneLights = false;
    bool usesAtmosphere = false;
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
    ShaderToneMappingPolicy toneMapping =
        ShaderToneMappingPolicy::PassThrough;
    std::string vertSpvPath;
    std::string fragSpvPath;
};

} // namespace vkr
