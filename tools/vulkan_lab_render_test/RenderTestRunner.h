#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vkr::render_test {

struct RenderTestRunOptions {
    std::filesystem::path specPath;
    std::filesystem::path runtimeExecutable;
    std::filesystem::path outputRoot;
    std::optional<std::filesystem::path> projectRoot;
    bool accept = false;
    uint32_t startupTimeoutMs = 30000;
    uint32_t operationTimeoutMs = 300000;
    uint32_t renderTimeoutMs = 30000;
    uint32_t captureTimeoutMs = 30000;
    uint32_t quitTimeoutMs = 10000;
};

struct RenderTestRunResult {
    int exitCode = 1;
    std::string status = "failed";
    std::string code;
    std::string message;
    std::filesystem::path resultRoot;
    std::filesystem::path reportPath;
};

RenderTestRunResult runRenderTest(const RenderTestRunOptions &options);

} // namespace vkr::render_test
