#include "ImageComparator.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {

void requireImage(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Exception, typename Callback>
void requireImageThrows(Callback callback, const char *message) {
    try {
        callback();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(message);
}

vkr::render_test::RgbaImage fixture() {
    vkr::render_test::RgbaImage image;
    image.width = 8;
    image.height = 8;
    image.pixels.resize(8 * 8 * 4, 255);
    for (size_t pixel = 0; pixel < 64; ++pixel) {
        const size_t offset = pixel * 4;
        if (pixel < 32) {
            image.pixels[offset + 0] = 0;
            image.pixels[offset + 1] = 0;
            image.pixels[offset + 2] = 0;
        } else {
            image.pixels[offset + 0] = 120;
            image.pixels[offset + 1] = 20;
            image.pixels[offset + 2] = 10;
        }
    }
    return image;
}

void testSmokeMetrics() {
    const auto image = fixture();
    const auto analysis = vkr::render_test::analyzeImage(image);
    requireImage(std::abs(analysis.nonBlackRatio - 0.5) < 1e-12 &&
                     std::abs(analysis.nonWhiteRatio - 1.0) < 1e-12 &&
                     std::abs(analysis.dominantSolidColorRatio - 0.5) <
                         1e-12,
                 "known 8x8 smoke metrics changed");
    vkr::render_test::SmokeThresholds passThresholds;
    passThresholds.minimumNonBlackRatio = 0.49;
    passThresholds.maximumSolidColorRatio = 0.51;
    const auto pass =
        vkr::render_test::evaluateSmoke(image, passThresholds);
    requireImage(pass.passed && pass.failures.empty(),
                 "valid smoke fixture failed");
    vkr::render_test::SmokeThresholds failThresholds;
    failThresholds.minimumNonBlackRatio = 0.75;
    failThresholds.maximumSolidColorRatio = 0.49;
    const auto fail =
        vkr::render_test::evaluateSmoke(image, failThresholds);
    requireImage(!fail.passed && fail.failures.size() == 2,
                 "smoke failures were not reported independently");
}

void testKnownGoldenMetricsAndDiff() {
    auto baseline = fixture();
    auto actual = baseline;
    actual.pixels[0] = 10;
    actual.pixels[1] = 20;
    actual.pixels[3] = 250;

    vkr::render_test::GoldenThresholds thresholds;
    thresholds.perChannelAbsoluteThreshold = {9, 19, 0, 4};
    thresholds.maximumMae = 0.2;
    thresholds.maximumRmse = 2.0;
    thresholds.maximumBadPixelRatio = 0.02;
    const auto comparison =
        vkr::render_test::compareImages(actual, baseline, thresholds);
    const double expectedMae = 35.0 / 256.0;
    const double expectedRmse = std::sqrt(525.0 / 256.0);
    requireImage(comparison.passed && comparison.metrics.dimensionsMatch &&
                     comparison.metrics.badPixelCount == 1 &&
                     std::abs(comparison.metrics.badPixelRatio - 1.0 / 64.0) <
                         1e-12 &&
                     std::abs(comparison.metrics.mae - expectedMae) < 1e-12 &&
                     std::abs(comparison.metrics.rmse - expectedRmse) <
                         1e-12 &&
                     comparison.metrics.maximumError == 20,
                 "known golden metrics changed");
    requireImage(comparison.diff.width == 8 &&
                     comparison.diff.height == 8 &&
                     comparison.diff.pixels[0] == 80 &&
                     comparison.diff.pixels[1] == 160 &&
                     comparison.diff.pixels[2] == 0 &&
                     comparison.diff.pixels[3] == 255,
                 "diff amplification or alpha changed");

    thresholds.maximumMae = 0.1;
    thresholds.maximumRmse = 1.0;
    thresholds.maximumBadPixelRatio = 0.0;
    const auto failed =
        vkr::render_test::compareImages(actual, baseline, thresholds);
    requireImage(!failed.passed && failed.failures.size() == 3,
                 "golden threshold failures were not reported");
}

void testDimensionMismatchAndPayloadValidation() {
    const auto baseline = fixture();
    auto actual = baseline;
    actual.width = 4;
    actual.height = 16;
    const auto mismatch = vkr::render_test::compareImages(
        actual, baseline, vkr::render_test::GoldenThresholds{});
    requireImage(!mismatch.passed &&
                     !mismatch.metrics.dimensionsMatch &&
                     mismatch.failures ==
                         std::vector<std::string>{"dimension_mismatch"} &&
                     mismatch.diff.pixels.empty(),
                 "dimension mismatch fabricated a diff or wrong result");

    auto invalid = fixture();
    invalid.pixels.pop_back();
    requireImageThrows<std::invalid_argument>(
        [&] { vkr::render_test::analyzeImage(invalid); },
        "invalid RGBA payload was accepted");

    vkr::render_test::RgbaImage enormous;
    enormous.width = std::numeric_limits<uint32_t>::max();
    enormous.height = std::numeric_limits<uint32_t>::max();
    requireImageThrows<std::overflow_error>(
        [&] { vkr::render_test::analyzeImage(enormous); },
        "RGBA extent overflow was accepted");
}

void testPngRoundTripAndAlpha() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path(
                          L"vulkan-lab-render-\u89c6\u89c9");
    std::filesystem::create_directories(root);
    const auto path = root / "fixture.png";
    auto image = fixture();
    image.pixels[3] = 17;
    vkr::render_test::writeRgbaPngAtomic(path, image);
    const auto decoded = vkr::render_test::loadRgbaPng(path);
    requireImage(decoded.width == image.width &&
                     decoded.height == image.height &&
                     decoded.pixels == image.pixels,
                 "PNG round trip changed RGBA bytes or Unicode path");
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    requireImage(!std::filesystem::exists(temporary),
                 "atomic PNG write left a temporary file");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

void runImageComparatorTests() {
    testSmokeMetrics();
    testKnownGoldenMetricsAndDiff();
    testDimensionMismatchAndPayloadValidation();
    testPngRoundTripAndAlpha();
}
