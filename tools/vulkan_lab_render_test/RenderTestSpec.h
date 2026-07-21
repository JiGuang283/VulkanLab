#pragma once

#include "render/RenderSettings.h"

#include <json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vkr::render_test {

enum class RenderTestMode { Smoke, Golden };

struct RenderTestCamera {
    std::array<float, 3> position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct SmokeThresholds {
    double minimumNonBlackRatio = 0.05;
    double maximumSolidColorRatio = 0.98;
};

struct GoldenThresholds {
    std::array<uint8_t, 4> perChannelAbsoluteThreshold{};
    double maximumMae = 0.0;
    double maximumRmse = 0.0;
    double maximumBadPixelRatio = 0.0;
};

struct GoldenSpec {
    std::filesystem::path baselineImage;
    std::filesystem::path baselineMetadata;
    GoldenThresholds thresholds;
};

struct RenderTestSpec {
    static constexpr uint32_t kSchemaVersion = 2;

    std::filesystem::path sourcePath;
    std::string name;
    std::string sceneId;
    std::string profileId;
    std::string shader;
    RenderTestCamera camera;
    std::array<uint32_t, 2> viewport{800, 600};
    double fixedDelta = 1.0 / 60.0;
    uint32_t stableFrames = 8;
    bool includeGui = false;
    RenderSettings renderSettings;
    RenderTestMode mode = RenderTestMode::Smoke;
    SmokeThresholds smokeThresholds;
    std::optional<GoldenSpec> golden;
};

RenderTestSpec parseRenderTestSpec(const nlohmann::json &document,
                                   const std::filesystem::path &sourcePath);
RenderTestSpec loadRenderTestSpec(const std::filesystem::path &path);
nlohmann::json renderTestSpecToJson(const RenderTestSpec &spec);
const char *renderTestModeName(RenderTestMode mode);

} // namespace vkr::render_test
