#include "diagnostics/DiagnosticsConfig.h"

#include <cmath>
#include <stdexcept>
#include <string_view>

namespace {

void requireDiagnostics(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Callback>
void requireInvalidDiagnostics(Callback callback, const char *message) {
    try {
        callback();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error(message);
}

void testDiagnosticsDefaults() {
    const vkr::DiagnosticsConfig config;
    requireDiagnostics(!config.automationMode,
                       "automation must be disabled by default");
    requireDiagnostics(!config.fixedDeltaSeconds,
                       "fixed delta must be disabled by default");
    requireDiagnostics(config.windowResizable(),
                       "the default window must remain resizable");
    requireDiagnostics(config.guiVisible,
                       "the GUI must remain visible by default");
    requireDiagnostics(config.runtimePipeSuffix.empty(),
                       "the default runtime pipe suffix must be empty");
    requireDiagnostics(config.captureRoot.empty(),
                       "the capture root must be resolved by ProjectContext");

    vkr::DiagnosticsConfig automation;
    automation.automationMode = true;
    requireDiagnostics(!automation.windowResizable(),
                       "automation windows must be fixed");

    vkr::DiagnosticsConfig fixedWindow;
    fixedWindow.fixedWindowSize = true;
    requireDiagnostics(!fixedWindow.windowResizable(),
                       "an explicit fixed window must not be resizable");
}

void testWindowSizeParser() {
    const auto regular = vkr::parseDiagnosticWindowSize("800x600");
    requireDiagnostics(regular.width == 800 && regular.height == 600,
                       "lowercase window size was parsed incorrectly");

    const auto uppercase = vkr::parseDiagnosticWindowSize("1920X1080");
    requireDiagnostics(uppercase.width == 1920 && uppercase.height == 1080,
                       "uppercase window size was parsed incorrectly");

    for (std::string_view invalid : {"", "800", "x600", "800x", "0x600",
                                     "800x0", "800x600x1", "20000x600",
                                     "800.5x600"}) {
        requireInvalidDiagnostics(
            [invalid]() { vkr::parseDiagnosticWindowSize(invalid); },
            "invalid window size was accepted");
    }
}

void testFixedDeltaParser() {
    const float value = vkr::parseDiagnosticFixedDelta("0.016666667");
    requireDiagnostics(std::abs(value - 1.0f / 60.0f) < 0.000001f,
                       "fixed delta was parsed incorrectly");

    for (std::string_view invalid : {"", "0", "-0.1", "1.1", "nan",
                                     "0.1s"}) {
        requireInvalidDiagnostics(
            [invalid]() { vkr::parseDiagnosticFixedDelta(invalid); },
            "invalid fixed delta was accepted");
    }
}

} // namespace

void runDiagnosticsConfigTests() {
    testDiagnosticsDefaults();
    testWindowSizeParser();
    testFixedDeltaParser();
}
