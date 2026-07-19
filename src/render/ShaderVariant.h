#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vkr {

enum class ShaderVariantId : uint32_t {
    LegacyForward = 0,
    PbrLiteForward = 1,
    PbrLiteNormalMapped = 2,
    DebugBaseColor = 3,
    DebugNormal = 4,
    DebugRoughness = 5,
    DebugMetallic = 6,
    DebugOcclusion = 7,
    DebugEmissive = 8,
    DebugAlpha = 9,
    DebugTransmission = 10,
};

struct ShaderVariant {
    ShaderVariantId id;
    const char     *displayName;
    std::string     vertSpvPath;
    std::string     fragSpvPath;
};

inline const std::array<ShaderVariant, 11> kShaderVariants = {{
    {ShaderVariantId::LegacyForward, "Legacy Forward",
     "shader/legacy/forward.vert.spv", "shader/legacy/forward.frag.spv"},
    {ShaderVariantId::PbrLiteForward, "PBR-lite Forward",
     "shader/pbr_lite/forward.vert.spv",
     "shader/pbr_lite/forward.frag.spv"},
    {ShaderVariantId::PbrLiteNormalMapped, "PBR-lite NormalMapped",
     "shader/pbr_lite/forward_normal_mapped.vert.spv",
     "shader/pbr_lite/forward_normal_mapped.frag.spv"},
    {ShaderVariantId::DebugBaseColor, "Debug BaseColor",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/base_color.frag.spv"},
    {ShaderVariantId::DebugNormal, "Debug Normal",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/normal.frag.spv"},
    {ShaderVariantId::DebugRoughness, "Debug Roughness",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/roughness.frag.spv"},
    {ShaderVariantId::DebugMetallic, "Debug Metallic",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/metallic.frag.spv"},
    {ShaderVariantId::DebugOcclusion, "Debug Occlusion",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/occlusion.frag.spv"},
    {ShaderVariantId::DebugEmissive, "Debug Emissive",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/emissive.frag.spv"},
    {ShaderVariantId::DebugAlpha, "Debug Alpha",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/alpha.frag.spv"},
    {ShaderVariantId::DebugTransmission, "Debug Transmission",
     "shader/material_debug/material.vert.spv",
     "shader/material_debug/transmission.frag.spv"},
}};

inline const ShaderVariant &defaultShaderVariant() {
    return kShaderVariants[0];
}

} // namespace vkr
