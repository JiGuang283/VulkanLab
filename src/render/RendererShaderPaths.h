#pragma once

#include <array>
#include <string>
#include <string_view>

namespace vkr {

inline constexpr std::string_view kShadowVertexShaderPath =
    "shader/shadow/depth.vert.spv";
inline constexpr std::string_view kShadowMaskFragmentShaderPath =
    "shader/shadow/depth_mask.frag.spv";
inline constexpr std::string_view kFullscreenVertexShaderPath =
    "shader/postprocess/fullscreen.vert.spv";
inline constexpr std::string_view kToneMapFragmentShaderPath =
    "shader/postprocess/tonemap.frag.spv";
inline constexpr std::array<std::string_view, 4> kRendererShaderPaths = {
    kShadowVertexShaderPath, kShadowMaskFragmentShaderPath,
    kFullscreenVertexShaderPath, kToneMapFragmentShaderPath};

struct RendererShaderPaths {
    std::string shadowVert;
    std::string shadowMaskFrag;
    std::string fullscreenVert;
    std::string toneMapFrag;
};

} // namespace vkr
