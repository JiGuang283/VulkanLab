#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vkr {

struct DiagnosticWindowSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DiagnosticsConfig {
    bool automationMode = false;
    std::optional<float> fixedDeltaSeconds;
    bool fixedWindowSize = false;
    bool guiVisible = true;
    std::string runtimePipeSuffix;
    std::filesystem::path captureRoot;

    bool windowResizable() const {
        return !automationMode && !fixedWindowSize;
    }
};

DiagnosticWindowSize parseDiagnosticWindowSize(std::string_view value);
float parseDiagnosticFixedDelta(std::string_view value);

} // namespace vkr
