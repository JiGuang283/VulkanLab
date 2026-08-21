#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include "core/MaterialBindingMode.h"

namespace vkr {

enum class ShaderProgramContract {
    MainForward,
    ShadowDepth,
    PunctualShadowDepth,
    SurfacePrepass,
    GBuffer,
    DeferredLighting,
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
    bool usesClusteredLighting = false;
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

enum class MaterialShaderPass : uint32_t {
    ForwardOpaque,
    ForwardTransparent,
    SurfaceOpaque,
    SurfaceMask,
    GBufferOpaque,
    GBufferMask,
    DirectionalShadowMask,
    PointShadowMask,
    SpotShadowMask,
    Count,
};

struct MaterialShaderFamilyHandle {
    static constexpr uint32_t kInvalidIndex =
        std::numeric_limits<uint32_t>::max();

    uint32_t index = kInvalidIndex;

    bool valid() const { return index != kInvalidIndex; }
    friend bool operator==(MaterialShaderFamilyHandle left,
                           MaterialShaderFamilyHandle right) {
        return left.index == right.index;
    }
    friend bool operator!=(MaterialShaderFamilyHandle left,
                           MaterialShaderFamilyHandle right) {
        return !(left == right);
    }
};

struct MaterialShaderFamily {
    std::string id;
    std::string displayName;
    std::string category;
    int32_t order = 0;
    bool isDefault = false;
    std::array<std::string,
               static_cast<size_t>(MaterialShaderPass::Count)>
        programIds{};

    const std::string &programId(MaterialShaderPass pass) const {
        return programIds.at(static_cast<size_t>(pass));
    }
};

struct ViewMode {
    std::string id;
    std::string displayName;
    // Empty means the active material shader family supplies the program.
    std::string overrideProgramId;
    std::string category;
    int32_t order = 0;
    bool isDefault = false;
    bool supportsBloom = false;
    bool supportsAtmosphere = false;
    bool supportsScreenSpace = false;
    bool supportsDdgi = false;
    bool supportsDeferred = false;
    ShaderToneMappingPolicy toneMapping =
        ShaderToneMappingPolicy::PassThrough;
    bool overridesMaterialShader() const {
        return !overrideProgramId.empty();
    }
};

} // namespace vkr
