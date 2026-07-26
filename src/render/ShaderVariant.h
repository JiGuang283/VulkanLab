#pragma once

#include <cstdint>
#include <string>

namespace vkr {

enum class ShaderProgramContract {
    MainForward,
    ShadowDepth,
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
};

struct ShaderVariant {
    std::string id;
    std::string displayName;
    std::string programId;
    std::string category;
    int32_t order = 0;
    bool isDefault = false;
    ShaderToneMappingPolicy toneMapping =
        ShaderToneMappingPolicy::PassThrough;
    std::string vertSpvPath;
    std::string fragSpvPath;
};

} // namespace vkr
