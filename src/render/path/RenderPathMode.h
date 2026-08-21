#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vkr {

enum class RenderPathRequest : uint32_t {
    Auto,
    Forward,
    Deferred,
};

enum class RenderPathMode : uint32_t {
    Forward,
    Deferred,
};

constexpr std::string_view renderPathRequestName(RenderPathRequest request) {
    switch (request) {
    case RenderPathRequest::Auto:
        return "auto";
    case RenderPathRequest::Forward:
        return "forward";
    case RenderPathRequest::Deferred:
        return "deferred";
    }
    return "unknown";
}

inline std::optional<RenderPathRequest>
renderPathRequestFromName(std::string_view name) {
    if (name == "auto")
        return RenderPathRequest::Auto;
    if (name == "forward")
        return RenderPathRequest::Forward;
    if (name == "deferred")
        return RenderPathRequest::Deferred;
    return std::nullopt;
}

constexpr std::string_view renderPathModeName(RenderPathMode mode) {
    switch (mode) {
    case RenderPathMode::Forward:
        return "forward";
    case RenderPathMode::Deferred:
        return "deferred";
    }
    return "unknown";
}

struct RenderPathCapabilities {
    bool forward = true;
    bool deferred = false;
    bool multisampledOpaque = false;
    bool forwardTransparent = true;
    bool gBuffer = false;
};

struct RenderPathSelection {
    RenderPathRequest requested = RenderPathRequest::Auto;
    RenderPathMode active = RenderPathMode::Forward;
    RenderPathCapabilities capabilities{};
    bool viewModeCompatible = true;
    std::string fallbackReason;
};

inline RenderPathSelection resolveRenderPath(
    RenderPathRequest requested, const RenderPathCapabilities &capabilities,
    bool viewModeCompatible) {
    RenderPathSelection result{};
    result.requested = requested;
    result.capabilities = capabilities;
    result.viewModeCompatible = viewModeCompatible;

    if (requested == RenderPathRequest::Forward) {
        result.active = RenderPathMode::Forward;
        return result;
    }
    if (!capabilities.deferred) {
        result.fallbackReason = "deferred rendering is not supported";
        return result;
    }
    if (!viewModeCompatible) {
        result.fallbackReason = "the active view mode is Forward-only";
        return result;
    }
    result.active = RenderPathMode::Deferred;
    return result;
}

} // namespace vkr
