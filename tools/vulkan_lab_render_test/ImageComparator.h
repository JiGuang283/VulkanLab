#pragma once

#include "RenderTestSpec.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vkr::render_test {

struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct ImageAnalysis {
    double nonBlackRatio = 0.0;
    double nonWhiteRatio = 0.0;
    double dominantSolidColorRatio = 0.0;
    std::array<uint8_t, 4> dominantColor{};
};

struct SmokeEvaluation {
    bool passed = false;
    ImageAnalysis analysis;
    std::vector<std::string> failures;
};

struct ImageComparisonMetrics {
    bool dimensionsMatch = false;
    double mae = 0.0;
    double rmse = 0.0;
    uint8_t maximumError = 0;
    std::array<double, 4> maePerChannel{};
    std::array<double, 4> rmsePerChannel{};
    std::array<uint8_t, 4> maximumErrorPerChannel{};
    uint64_t badPixelCount = 0;
    double badPixelRatio = 0.0;
};

struct GoldenEvaluation {
    bool passed = false;
    ImageComparisonMetrics metrics;
    std::vector<std::string> failures;
    RgbaImage diff;
};

RgbaImage loadRgbaPng(const std::filesystem::path &path);
void writeRgbaPngAtomic(const std::filesystem::path &path,
                        const RgbaImage &image);
ImageAnalysis analyzeImage(const RgbaImage &image);
SmokeEvaluation evaluateSmoke(const RgbaImage &image,
                              const SmokeThresholds &thresholds);
GoldenEvaluation compareImages(const RgbaImage &actual,
                               const RgbaImage &baseline,
                               const GoldenThresholds &thresholds,
                               uint32_t diffAmplification = 8);

} // namespace vkr::render_test
