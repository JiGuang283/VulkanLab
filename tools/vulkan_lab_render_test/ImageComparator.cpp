#include "ImageComparator.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr::render_test {
namespace {

constexpr uint64_t kMaximumImageBytes = 1024ull * 1024ull * 1024ull;
constexpr uint8_t kBlackThreshold = 8;
constexpr uint8_t kWhiteThreshold = 247;

size_t checkedImageBytes(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        throw std::invalid_argument("RGBA image dimensions must be non-zero");
    const uint64_t pixelCount = static_cast<uint64_t>(width) * height;
    if (pixelCount > std::numeric_limits<uint64_t>::max() / 4)
        throw std::overflow_error("RGBA image size overflow");
    const uint64_t bytes = pixelCount * 4;
    if (bytes > kMaximumImageBytes || bytes > SIZE_MAX)
        throw std::length_error("RGBA image exceeds the supported byte limit");
    return static_cast<size_t>(bytes);
}

void validateImage(const RgbaImage &image) {
    if (image.pixels.size() != checkedImageBytes(image.width, image.height))
        throw std::invalid_argument(
            "RGBA image payload does not match its dimensions");
}

std::vector<uint8_t> readFile(const std::filesystem::path &path) {
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumImageBytes || size > SIZE_MAX)
        throw std::runtime_error("could not read PNG: " + path.u8string());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open PNG: " + path.u8string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()))
        throw std::runtime_error("could not read complete PNG: " +
                                 path.u8string());
    return bytes;
}

void appendPngBytes(void *context, void *data, int size) {
    if (!context || !data || size <= 0)
        return;
    auto &bytes = *static_cast<std::vector<uint8_t> *>(context);
    const auto *first = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), first, first + size);
}

void atomicPublish(const std::filesystem::path &temporary,
                   const std::filesystem::path &output) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("could not publish PNG (Win32 error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    std::filesystem::rename(temporary, output);
#endif
}

uint32_t packColor(const uint8_t *pixel) {
    return static_cast<uint32_t>(pixel[0]) |
           (static_cast<uint32_t>(pixel[1]) << 8u) |
           (static_cast<uint32_t>(pixel[2]) << 16u) |
           (static_cast<uint32_t>(pixel[3]) << 24u);
}

std::array<uint8_t, 4> unpackColor(uint32_t color) {
    return {static_cast<uint8_t>(color),
            static_cast<uint8_t>(color >> 8u),
            static_cast<uint8_t>(color >> 16u),
            static_cast<uint8_t>(color >> 24u)};
}

} // namespace

RgbaImage loadRgbaPng(const std::filesystem::path &path) {
    const std::vector<uint8_t> encoded = readFile(path);
    if (encoded.size() > static_cast<size_t>(INT_MAX))
        throw std::length_error("PNG encoded payload exceeds stb limits");
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()), &width, &height,
        &channels, STBI_rgb_alpha);
    if (!decoded)
        throw std::runtime_error("could not decode PNG: " + path.u8string());
    try {
        if (width <= 0 || height <= 0)
            throw std::runtime_error("decoded PNG has invalid dimensions");
        RgbaImage result;
        result.width = static_cast<uint32_t>(width);
        result.height = static_cast<uint32_t>(height);
        const size_t bytes = checkedImageBytes(result.width, result.height);
        result.pixels.assign(decoded, decoded + bytes);
        stbi_image_free(decoded);
        return result;
    } catch (...) {
        stbi_image_free(decoded);
        throw;
    }
}

void writeRgbaPngAtomic(const std::filesystem::path &path,
                        const RgbaImage &image) {
    validateImage(image);
    if (image.width > static_cast<uint32_t>(INT_MAX) ||
        image.height > static_cast<uint32_t>(INT_MAX))
        throw std::length_error("RGBA image dimensions exceed stb limits");
    std::vector<uint8_t> encoded;
    if (!stbi_write_png_to_func(
            appendPngBytes, &encoded, static_cast<int>(image.width),
            static_cast<int>(image.height), 4, image.pixels.data(),
            static_cast<int>(image.width * 4u))) {
        throw std::runtime_error("could not encode PNG");
    }
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("could not create temporary PNG");
        output.write(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        output.close();
        if (!output)
            throw std::runtime_error("could not write temporary PNG");
        atomicPublish(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

ImageAnalysis analyzeImage(const RgbaImage &image) {
    validateImage(image);
    const uint64_t pixelCount =
        static_cast<uint64_t>(image.width) * image.height;
    uint64_t nonBlack = 0;
    uint64_t nonWhite = 0;
    std::unordered_map<uint32_t, uint64_t> colors;
    colors.reserve(static_cast<size_t>(std::min<uint64_t>(pixelCount, 65536)));
    uint64_t dominantCount = 0;
    uint32_t dominantColor = 0;
    for (size_t offset = 0; offset < image.pixels.size(); offset += 4) {
        const uint8_t *pixel = image.pixels.data() + offset;
        if (pixel[0] > kBlackThreshold || pixel[1] > kBlackThreshold ||
            pixel[2] > kBlackThreshold)
            ++nonBlack;
        if (pixel[0] < kWhiteThreshold || pixel[1] < kWhiteThreshold ||
            pixel[2] < kWhiteThreshold)
            ++nonWhite;
        const uint32_t packed = packColor(pixel);
        const uint64_t count = ++colors[packed];
        if (count > dominantCount) {
            dominantCount = count;
            dominantColor = packed;
        }
    }
    ImageAnalysis result;
    result.nonBlackRatio = static_cast<double>(nonBlack) / pixelCount;
    result.nonWhiteRatio = static_cast<double>(nonWhite) / pixelCount;
    result.dominantSolidColorRatio =
        static_cast<double>(dominantCount) / pixelCount;
    result.dominantColor = unpackColor(dominantColor);
    return result;
}

SmokeEvaluation evaluateSmoke(const RgbaImage &image,
                              const SmokeThresholds &thresholds) {
    if (thresholds.minimumNonBlackRatio < 0.0 ||
        thresholds.minimumNonBlackRatio > 1.0 ||
        thresholds.maximumSolidColorRatio < 0.0 ||
        thresholds.maximumSolidColorRatio > 1.0) {
        throw std::invalid_argument("smoke thresholds must be in [0, 1]");
    }
    SmokeEvaluation result;
    result.analysis = analyzeImage(image);
    if (result.analysis.nonBlackRatio < thresholds.minimumNonBlackRatio)
        result.failures.push_back("minimum_non_black_ratio");
    if (result.analysis.dominantSolidColorRatio >
        thresholds.maximumSolidColorRatio)
        result.failures.push_back("maximum_solid_color_ratio");
    result.passed = result.failures.empty();
    return result;
}

GoldenEvaluation compareImages(const RgbaImage &actual,
                               const RgbaImage &baseline,
                               const GoldenThresholds &thresholds,
                               uint32_t diffAmplification) {
    validateImage(actual);
    validateImage(baseline);
    if (thresholds.maximumMae < 0.0 ||
        thresholds.maximumRmse < 0.0 ||
        thresholds.maximumBadPixelRatio < 0.0 ||
        thresholds.maximumBadPixelRatio > 1.0 ||
        diffAmplification == 0) {
        throw std::invalid_argument("golden thresholds are invalid");
    }

    GoldenEvaluation result;
    result.metrics.dimensionsMatch =
        actual.width == baseline.width && actual.height == baseline.height;
    if (!result.metrics.dimensionsMatch) {
        result.failures.push_back("dimension_mismatch");
        return result;
    }

    result.diff.width = actual.width;
    result.diff.height = actual.height;
    result.diff.pixels.resize(actual.pixels.size(), 255);
    const uint64_t pixelCount =
        static_cast<uint64_t>(actual.width) * actual.height;
    std::array<uint64_t, 4> channelAbsolute{};
    std::array<uint64_t, 4> channelSquared{};
    uint64_t totalAbsolute = 0;
    uint64_t totalSquared = 0;

    for (uint64_t pixel = 0; pixel < pixelCount; ++pixel) {
        bool bad = false;
        const size_t offset = static_cast<size_t>(pixel * 4);
        for (size_t channel = 0; channel < 4; ++channel) {
            const uint8_t difference = static_cast<uint8_t>(std::abs(
                static_cast<int>(actual.pixels[offset + channel]) -
                static_cast<int>(baseline.pixels[offset + channel])));
            channelAbsolute[channel] += difference;
            channelSquared[channel] +=
                static_cast<uint64_t>(difference) * difference;
            totalAbsolute += difference;
            totalSquared += static_cast<uint64_t>(difference) * difference;
            result.metrics.maximumErrorPerChannel[channel] =
                std::max(result.metrics.maximumErrorPerChannel[channel],
                         difference);
            result.metrics.maximumError =
                std::max(result.metrics.maximumError, difference);
            if (difference >
                thresholds.perChannelAbsoluteThreshold[channel])
                bad = true;
            if (channel < 3) {
                result.diff.pixels[offset + channel] =
                    static_cast<uint8_t>(std::min<uint32_t>(
                        255, static_cast<uint32_t>(difference) *
                                 diffAmplification));
            }
        }
        result.diff.pixels[offset + 3] = 255;
        if (bad)
            ++result.metrics.badPixelCount;
    }

    const double channelSamples = static_cast<double>(pixelCount) * 4.0;
    result.metrics.mae = static_cast<double>(totalAbsolute) / channelSamples;
    result.metrics.rmse =
        std::sqrt(static_cast<double>(totalSquared) / channelSamples);
    result.metrics.badPixelRatio =
        static_cast<double>(result.metrics.badPixelCount) / pixelCount;
    for (size_t channel = 0; channel < 4; ++channel) {
        result.metrics.maePerChannel[channel] =
            static_cast<double>(channelAbsolute[channel]) / pixelCount;
        result.metrics.rmsePerChannel[channel] = std::sqrt(
            static_cast<double>(channelSquared[channel]) / pixelCount);
    }

    if (result.metrics.mae > thresholds.maximumMae)
        result.failures.push_back("maximum_mae");
    if (result.metrics.rmse > thresholds.maximumRmse)
        result.failures.push_back("maximum_rmse");
    if (result.metrics.badPixelRatio > thresholds.maximumBadPixelRatio)
        result.failures.push_back("maximum_bad_pixel_ratio");
    result.passed = result.failures.empty();
    return result;
}

} // namespace vkr::render_test
