#include "RenderTestSpec.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

void requireSpec(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Callback>
void requireInvalidSpec(Callback callback, const char *message) {
    try {
        callback();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error(message);
}

Json validSpec() {
    return {
        {"schemaVersion", 1},
        {"name", "renderer-smoke-legacy"},
        {"sceneId", "renderer-smoke"},
        {"profileId", "desktop_2048"},
        {"shader", "Legacy Forward"},
        {"camera",
         {{"position", {2.0, 2.0, 2.0}},
          {"yaw", -135.0},
          {"pitch", -30.0}}},
        {"viewport", {800, 600}},
        {"fixedDelta", 0.016666667},
        {"stableFrames", 8},
        {"includeGui", false},
        {"mode", "smoke"},
        {"thresholds",
         {{"minimumNonBlackRatio", 0.05},
          {"maximumSolidColorRatio", 0.98}}},
    };
}

void testValidSmokeSpec() {
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "renderer-smoke.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(validSpec(), source);
    requireSpec(spec.name == "renderer-smoke-legacy" &&
                    spec.sceneId == "renderer-smoke" &&
                    spec.profileId == "desktop_2048" &&
                    spec.shader == "Legacy Forward",
                "valid render test identity changed");
    requireSpec(spec.viewport == std::array<uint32_t, 2>{800, 600} &&
                    spec.stableFrames == 8 && !spec.includeGui &&
                    spec.mode == vkr::render_test::RenderTestMode::Smoke &&
                    !spec.golden,
                "valid smoke fields changed");
    requireSpec(std::abs(spec.camera.position[0] - 2.0f) < 0.0001f &&
                    std::abs(spec.camera.yaw + 135.0f) < 0.0001f,
                "camera values changed during spec parsing");
    requireSpec(spec.renderSettings.shadowsEnabled &&
                    spec.renderSettings.toneMapper == vkr::ToneMapper::Aces &&
                    std::abs(spec.renderSettings.exposureEv) < 0.0001f,
                "schema v1 did not retain default render settings");
}

void testNativeSceneSpecWithoutProfile() {
    Json document = validSpec();
    document.erase("profileId");
    const auto source = std::filesystem::temp_directory_path() /
                        "render-tests" / "native-scene.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(document, source);
    requireSpec(spec.profileId.empty(),
                "native scene spec invented a model profile");
    const Json serialized =
        vkr::render_test::renderTestSpecToJson(spec);
    requireSpec(!serialized.contains("profileId"),
                "native scene spec serialized an empty model profile");
}

void testSchemaV2RenderSettings() {
    Json document = validSpec();
    document["schemaVersion"] = 2;
    document["renderSettings"] = {
        {"shadowsEnabled", false},
        {"shadowReceiverBias", 0.002f},
        {"shadowConstantBias", 2.0f},
        {"shadowSlopeBias", 3.0f},
        {"exposureEv", 1.5f},
        {"toneMapper", "reinhard"},
    };
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "renderer-smoke-v2.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(document, source);
    requireSpec(!spec.renderSettings.shadowsEnabled &&
                    std::abs(spec.renderSettings.shadowReceiverBias -
                             0.002f) < 0.0001f &&
                    std::abs(spec.renderSettings.exposureEv - 1.5f) <
                        0.0001f &&
                    spec.renderSettings.toneMapper ==
                        vkr::ToneMapper::Reinhard,
                "schema v2 render settings changed during parsing");

    const Json serialized =
        vkr::render_test::renderTestSpecToJson(spec);
    requireSpec(serialized["schemaVersion"] == 4 &&
                    serialized["renderSettings"]["toneMapper"] ==
                        "reinhard" &&
                    serialized["renderSettings"]["shadowsEnabled"] ==
                        false,
                "schema v2 render settings were not serialized");
}

void testSchemaV3Environment() {
    Json document = validSpec();
    document["schemaVersion"] = 3;
    document["environmentId"] = "studio-hdr";
    document["renderSettings"] = {
        {"iblEnabled", true},
        {"skyboxEnabled", true},
        {"environmentIntensity", 1.5f},
        {"environmentRotationRadians", 0.75f}};
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "environment-v3.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(document, source);
    requireSpec(
        spec.environmentId == std::optional<std::string>("studio-hdr") &&
            spec.renderSettings.iblEnabled &&
            spec.renderSettings.skyboxEnabled &&
            std::abs(spec.renderSettings.environmentIntensity - 1.5f) <
                0.0001f,
        "schema v3 environment fields changed during parsing");
    const Json serialized =
        vkr::render_test::renderTestSpecToJson(spec);
    requireSpec(serialized["schemaVersion"] == 4 &&
                    serialized["environmentId"] == "studio-hdr" &&
                    serialized["renderSettings"]["iblEnabled"] == true,
                "schema v3 environment fields were not serialized");
}

void testSchemaV4Bloom() {
    Json document = validSpec();
    document["schemaVersion"] = 4;
    document["renderSettings"] = {
        {"bloomEnabled", true},
        {"bloomThreshold", 1.25f},
        {"bloomSoftKnee", 0.4f},
        {"bloomIntensity", 0.2f},
    };
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "bloom-v4.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(document, source);
    requireSpec(
        spec.renderSettings.bloomEnabled &&
            std::abs(spec.renderSettings.bloomThreshold - 1.25f) <
                0.0001f &&
            std::abs(spec.renderSettings.bloomSoftKnee - 0.4f) <
                0.0001f &&
            std::abs(spec.renderSettings.bloomIntensity - 0.2f) <
                0.0001f,
        "schema v4 Bloom fields changed during parsing");
    const Json serialized =
        vkr::render_test::renderTestSpecToJson(spec);
    requireSpec(
        serialized["schemaVersion"] == 4 &&
            serialized["renderSettings"]["bloomEnabled"] == true &&
            serialized["renderSettings"]["bloomThreshold"] == 1.25f,
        "schema v4 Bloom fields were not serialized");
}

void testValidGoldenSpecAndResolvedPaths() {
    Json document = validSpec();
    document["mode"] = "golden";
    document["golden"] = {
        {"baselineImage", "goldens/renderer-smoke.png"},
        {"baselineMetadata", "goldens/renderer-smoke.json"},
        {"perChannelAbsoluteThreshold", {2, 3, 4, 0}},
        {"maximumMae", 1.25},
        {"maximumRmse", 2.5},
        {"maximumBadPixelRatio", 0.01},
    };
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "renderer-smoke-golden.json";
    const auto spec =
        vkr::render_test::parseRenderTestSpec(document, source);
    requireSpec(spec.golden.has_value() &&
                    spec.golden->baselineImage ==
                        std::filesystem::absolute(source.parent_path() /
                                                  "goldens/renderer-smoke.png")
                            .lexically_normal() &&
                    spec.golden->thresholds.perChannelAbsoluteThreshold ==
                        std::array<uint8_t, 4>{2, 3, 4, 0},
                "golden paths or thresholds were not resolved correctly");
}

void testStrictValidation() {
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "strict.json";

    Json unknown = validSpec();
    unknown["unexpected"] = true;
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(unknown, source); },
        "unknown top-level spec field was accepted");

    Json settingsInV1 = validSpec();
    settingsInV1["renderSettings"] = Json::object();
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(settingsInV1, source);
        },
        "schema v1 accepted renderSettings");

    Json badSettings = validSpec();
    badSettings["schemaVersion"] = 2;
    badSettings["renderSettings"] = {{"exposureEv", 100.0}};
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(badSettings, source);
        },
        "out-of-range exposure was accepted");

    badSettings["renderSettings"] = {{"toneMapper", "unknown"}};
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(badSettings, source);
        },
        "unknown tone mapper was accepted");

    Json environmentInV2 = validSpec();
    environmentInV2["schemaVersion"] = 2;
    environmentInV2["environmentId"] = "studio-hdr";
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(environmentInV2, source);
        },
        "schema v2 accepted environmentId");

    Json badCamera = validSpec();
    badCamera["camera"]["yaw"] =
        std::numeric_limits<double>::infinity();
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(badCamera, source); },
        "non-finite camera value was accepted");

    Json badViewport = validSpec();
    badViewport["viewport"] = {0, 600};
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(badViewport, source); },
        "zero viewport extent was accepted");

    badViewport["viewport"] = {-1, 600};
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(badViewport, source); },
        "negative viewport extent was accepted");

    badViewport["viewport"] =
        {std::numeric_limits<uint64_t>::max(), 600};
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(badViewport, source); },
        "oversized unsigned viewport extent was accepted");

    Json smokeWithGolden = validSpec();
    smokeWithGolden["golden"] = Json::object();
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(smokeWithGolden, source);
        },
        "smoke spec accepted golden configuration");

    Json goldenWithoutConfig = validSpec();
    goldenWithoutConfig["mode"] = "golden";
    requireInvalidSpec(
        [&] {
            vkr::render_test::parseRenderTestSpec(goldenWithoutConfig,
                                                  source);
        },
        "golden mode accepted missing configuration");
}

void testGoldenPathConfinement() {
    Json document = validSpec();
    document["mode"] = "golden";
    document["golden"] = {
        {"baselineImage", "../escape.png"},
        {"baselineMetadata", "goldens/reference.json"},
        {"perChannelAbsoluteThreshold", {0, 0, 0, 0}},
        {"maximumMae", 0.0},
        {"maximumRmse", 0.0},
        {"maximumBadPixelRatio", 0.0},
    };
    const auto source =
        std::filesystem::temp_directory_path() / "render-tests" /
        "escape.json";
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(document, source); },
        "golden path traversal was accepted");

    document["golden"]["baselineImage"] = "goldens/reference.jpg";
    requireInvalidSpec(
        [&] { vkr::render_test::parseRenderTestSpec(document, source); },
        "non-PNG golden image was accepted");
}

} // namespace

void runRenderTestSpecTests() {
    testValidSmokeSpec();
    testNativeSceneSpecWithoutProfile();
    testSchemaV2RenderSettings();
    testSchemaV3Environment();
    testSchemaV4Bloom();
    testValidGoldenSpecAndResolvedPaths();
    testStrictValidation();
    testGoldenPathConfinement();
}
