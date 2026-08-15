#pragma once

#include <array>
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
    std::string colorOnlyFragSpvPath;
    std::string bindlessColorOnlyFragSpvPath;
    std::string specularFragSpvPath;
    std::string bindlessSpecularFragSpvPath;
    std::array<std::string, 3> reducedSurfaceFragSpvPaths;
    std::array<std::string, 3> bindlessReducedSurfaceFragSpvPaths;
    std::string computeSpvPath;
    bool usesSceneLights = false;
    bool usesAtmosphere = false;
    bool usesScreenSpace = false;
    bool usesDdgi = false;
    bool usesMaterialTextures = false;
    bool usesLightingMrt = false;
    bool usesSurfaceMrt = false;

    const std::string &fragmentSpvPath(MaterialBindingMode mode,
                                       uint32_t colorAttachmentCount = 3) const {
        const bool bindless = mode == MaterialBindingMode::Bindless;
        if (usesSurfaceMrt && colorAttachmentCount < 3) {
            const auto &paths = bindless
                                    ? bindlessReducedSurfaceFragSpvPaths
                                    : reducedSurfaceFragSpvPaths;
            return paths[colorAttachmentCount];
        }
        if (usesLightingMrt && colorAttachmentCount <= 1) {
            return bindless && !bindlessColorOnlyFragSpvPath.empty()
                       ? bindlessColorOnlyFragSpvPath
                       : colorOnlyFragSpvPath;
        }
        if (usesLightingMrt && colorAttachmentCount == 2) {
            return bindless && !bindlessSpecularFragSpvPath.empty()
                       ? bindlessSpecularFragSpvPath
                       : specularFragSpvPath;
        }
        return bindless && !bindlessFragSpvPath.empty()
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
    std::string colorOnlyFragSpvPath;
    std::string bindlessColorOnlyFragSpvPath;
    std::string specularFragSpvPath;
    std::string bindlessSpecularFragSpvPath;
    bool usesLightingMrt = false;

    const std::string &fragmentSpvPath(MaterialBindingMode mode,
                                       uint32_t colorAttachmentCount = 3) const {
        const bool bindless = mode == MaterialBindingMode::Bindless;
        if (usesLightingMrt && colorAttachmentCount <= 1) {
            return bindless && !bindlessColorOnlyFragSpvPath.empty()
                       ? bindlessColorOnlyFragSpvPath
                       : colorOnlyFragSpvPath;
        }
        if (usesLightingMrt && colorAttachmentCount == 2) {
            return bindless && !bindlessSpecularFragSpvPath.empty()
                       ? bindlessSpecularFragSpvPath
                       : specularFragSpvPath;
        }
        return bindless && !bindlessFragSpvPath.empty()
                   ? bindlessFragSpvPath
                   : fragSpvPath;
    }
};

} // namespace vkr
