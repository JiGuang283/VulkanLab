#include "RenderTestSpec.h"

#include "assets/SceneCatalog.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace vkr::render_test {
namespace {

constexpr uint64_t kMaximumSpecBytes = 1024 * 1024;

void requireObject(const nlohmann::json &value, std::string_view context) {
    if (!value.is_object())
        throw std::invalid_argument(std::string(context) +
                                    " must be an object");
}

void rejectUnknownKeys(const nlohmann::json &value,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view context) {
    std::set<std::string> allowedSet;
    for (const std::string_view key : allowed)
        allowedSet.emplace(key);
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (allowedSet.count(it.key()) == 0) {
            throw std::invalid_argument(std::string(context) +
                                        " contains unknown field '" +
                                        it.key() + "'");
        }
    }
}

const nlohmann::json &required(const nlohmann::json &object,
                               const char *key) {
    const auto found = object.find(key);
    if (found == object.end())
        throw std::invalid_argument(std::string("missing required field '") +
                                    key + "'");
    return *found;
}

std::string requiredString(const nlohmann::json &object, const char *key,
                           size_t maximumLength = 256) {
    const nlohmann::json &value = required(object, key);
    if (!value.is_string())
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must be a string");
    std::string result = value.get<std::string>();
    if (result.empty() || result.size() > maximumLength)
        throw std::invalid_argument(std::string("field '") + key +
                                    "' has an invalid length");
    if (std::any_of(result.begin(), result.end(), [](unsigned char c) {
            return c < 0x20;
        })) {
        throw std::invalid_argument(std::string("field '") + key +
                                    "' contains a control character");
    }
    return result;
}

double requiredFinite(const nlohmann::json &object, const char *key) {
    const nlohmann::json &value = required(object, key);
    if (!value.is_number())
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must be a number");
    const double result = value.get<double>();
    if (!std::isfinite(result))
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must be finite");
    return result;
}

double requiredRatio(const nlohmann::json &object, const char *key) {
    const double result = requiredFinite(object, key);
    if (result < 0.0 || result > 1.0)
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must be in [0, 1]");
    return result;
}

uint32_t parseUint32(const nlohmann::json &value, std::string_view field,
                     uint32_t minimum, uint32_t maximum) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string("field '") +
                                    std::string(field) +
                                    "' must be an unsigned integer");
    uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<uint64_t>();
    } else {
        const int64_t signedResult = value.get<int64_t>();
        if (signedResult < 0)
            throw std::invalid_argument(std::string("field '") +
                                        std::string(field) +
                                        "' must be an unsigned integer");
        result = static_cast<uint64_t>(signedResult);
    }
    if (result < minimum || result > maximum)
        throw std::invalid_argument(std::string("field '") +
                                    std::string(field) +
                                    "' is outside the supported range");
    return static_cast<uint32_t>(result);
}

uint32_t requiredUint32(const nlohmann::json &object, const char *key,
                        uint32_t minimum, uint32_t maximum) {
    return parseUint32(required(object, key), key, minimum, maximum);
}

std::filesystem::path resolveSpecPath(const nlohmann::json &object,
                                      const char *key,
                                      const std::filesystem::path &sourcePath,
                                      std::string_view extension) {
    const std::filesystem::path relative =
        std::filesystem::u8path(requiredString(object, key, 1024));
    if (relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must be a relative path");
    }
    const std::filesystem::path normalized = relative.lexically_normal();
    for (const auto &component : normalized) {
        if (component == "..")
            throw std::invalid_argument(std::string("field '") + key +
                                        "' escapes the spec directory");
    }
    std::string actualExtension = normalized.extension().string();
    std::transform(actualExtension.begin(), actualExtension.end(),
                   actualExtension.begin(), [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (actualExtension != extension)
        throw std::invalid_argument(std::string("field '") + key +
                                    "' must use the " +
                                    std::string(extension) + " extension");
    const std::filesystem::path base =
        std::filesystem::absolute(sourcePath).parent_path();
    const std::filesystem::path resolved =
        std::filesystem::absolute(base / normalized).lexically_normal();
    if (!pathIsWithin(base, resolved))
        throw std::invalid_argument(std::string("field '") + key +
                                    "' escapes the spec directory");
    return resolved;
}

std::array<float, 3> parsePosition(const nlohmann::json &camera) {
    const nlohmann::json &value = required(camera, "position");
    if (!value.is_array() || value.size() != 3)
        throw std::invalid_argument(
            "field 'camera.position' must have three numbers");
    std::array<float, 3> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        if (!value[index].is_number())
            throw std::invalid_argument(
                "field 'camera.position' must have three numbers");
        const double component = value[index].get<double>();
        if (!std::isfinite(component) ||
            component < -std::numeric_limits<float>::max() ||
            component > std::numeric_limits<float>::max()) {
            throw std::invalid_argument(
                "field 'camera.position' must contain finite floats");
        }
        result[index] = static_cast<float>(component);
    }
    return result;
}

std::array<uint32_t, 2> parseViewport(const nlohmann::json &document) {
    const nlohmann::json &value = required(document, "viewport");
    if (!value.is_array() || value.size() != 2 ||
        !value[0].is_number_integer() ||
        !value[1].is_number_integer()) {
        throw std::invalid_argument(
            "field 'viewport' must contain two unsigned integers");
    }
    std::array<uint32_t, 2> result{};
    for (size_t index = 0; index < result.size(); ++index)
        result[index] = parseUint32(
            value[index], "viewport[" + std::to_string(index) + "]", 1,
            16384);
    return result;
}

std::array<uint8_t, 4>
parseAbsoluteThreshold(const nlohmann::json &golden) {
    const nlohmann::json &value =
        required(golden, "perChannelAbsoluteThreshold");
    if (!value.is_array() || value.size() != 4)
        throw std::invalid_argument(
            "field 'perChannelAbsoluteThreshold' must contain four bytes");
    std::array<uint8_t, 4> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<uint8_t>(parseUint32(
            value[index], "perChannelAbsoluteThreshold[" +
                              std::to_string(index) +
                              "]",
            0, 255));
    }
    return result;
}

} // namespace

const char *renderTestModeName(RenderTestMode mode) {
    return mode == RenderTestMode::Golden ? "golden" : "smoke";
}

RenderTestSpec parseRenderTestSpec(const nlohmann::json &document,
                                   const std::filesystem::path &sourcePath) {
    requireObject(document, "render test spec");
    rejectUnknownKeys(document,
                      {"schemaVersion", "name", "sceneId", "profileId",
                       "shader", "camera", "viewport", "fixedDelta",
                       "stableFrames", "includeGui", "mode", "thresholds",
                       "golden"},
                      "render test spec");

    RenderTestSpec spec;
    spec.sourcePath = std::filesystem::absolute(sourcePath).lexically_normal();
    const uint32_t schemaVersion =
        requiredUint32(document, "schemaVersion", 1, UINT32_MAX);
    if (schemaVersion != RenderTestSpec::kSchemaVersion)
        throw std::invalid_argument("unsupported render test schemaVersion");
    spec.name = requiredString(document, "name", 128);
    spec.sceneId = requiredString(document, "sceneId", 128);
    spec.profileId = requiredString(document, "profileId", 128);
    if (!isStableAssetId(spec.sceneId) || !isStableAssetId(spec.profileId))
        throw std::invalid_argument(
            "sceneId and profileId must be stable lowercase asset IDs");
    spec.shader = requiredString(document, "shader", 256);

    const nlohmann::json &camera = required(document, "camera");
    requireObject(camera, "camera");
    rejectUnknownKeys(camera, {"position", "yaw", "pitch"}, "camera");
    spec.camera.position = parsePosition(camera);
    const double yaw = requiredFinite(camera, "yaw");
    const double pitch = requiredFinite(camera, "pitch");
    if (yaw < -std::numeric_limits<float>::max() ||
        yaw > std::numeric_limits<float>::max() ||
        pitch < -std::numeric_limits<float>::max() ||
        pitch > std::numeric_limits<float>::max()) {
        throw std::invalid_argument("camera angles must fit finite floats");
    }
    spec.camera.yaw = static_cast<float>(yaw);
    spec.camera.pitch = static_cast<float>(pitch);
    spec.viewport = parseViewport(document);
    spec.fixedDelta = requiredFinite(document, "fixedDelta");
    if (spec.fixedDelta <= 0.0 || spec.fixedDelta > 1.0)
        throw std::invalid_argument("fixedDelta must be in (0, 1]");
    spec.stableFrames =
        requiredUint32(document, "stableFrames", 1, 100000);

    const nlohmann::json &includeGui = required(document, "includeGui");
    if (!includeGui.is_boolean())
        throw std::invalid_argument("field 'includeGui' must be a boolean");
    spec.includeGui = includeGui.get<bool>();

    const std::string mode = requiredString(document, "mode", 16);
    if (mode == "smoke")
        spec.mode = RenderTestMode::Smoke;
    else if (mode == "golden")
        spec.mode = RenderTestMode::Golden;
    else
        throw std::invalid_argument("mode must be 'smoke' or 'golden'");

    const nlohmann::json &thresholds = required(document, "thresholds");
    requireObject(thresholds, "thresholds");
    rejectUnknownKeys(thresholds,
                      {"minimumNonBlackRatio",
                       "maximumSolidColorRatio"},
                      "thresholds");
    spec.smokeThresholds.minimumNonBlackRatio =
        requiredRatio(thresholds, "minimumNonBlackRatio");
    spec.smokeThresholds.maximumSolidColorRatio =
        requiredRatio(thresholds, "maximumSolidColorRatio");

    const auto goldenIt = document.find("golden");
    if (spec.mode == RenderTestMode::Golden) {
        if (goldenIt == document.end())
            throw std::invalid_argument(
                "golden mode requires the 'golden' object");
        requireObject(*goldenIt, "golden");
        rejectUnknownKeys(*goldenIt,
                          {"baselineImage", "baselineMetadata",
                           "perChannelAbsoluteThreshold", "maximumMae",
                           "maximumRmse", "maximumBadPixelRatio"},
                          "golden");
        GoldenSpec golden;
        golden.baselineImage = resolveSpecPath(
            *goldenIt, "baselineImage", spec.sourcePath, ".png");
        golden.baselineMetadata = resolveSpecPath(
            *goldenIt, "baselineMetadata", spec.sourcePath, ".json");
        golden.thresholds.perChannelAbsoluteThreshold =
            parseAbsoluteThreshold(*goldenIt);
        golden.thresholds.maximumMae =
            requiredFinite(*goldenIt, "maximumMae");
        golden.thresholds.maximumRmse =
            requiredFinite(*goldenIt, "maximumRmse");
        golden.thresholds.maximumBadPixelRatio =
            requiredRatio(*goldenIt, "maximumBadPixelRatio");
        if (golden.thresholds.maximumMae < 0.0 ||
            golden.thresholds.maximumRmse < 0.0)
            throw std::invalid_argument(
                "golden MAE and RMSE limits must be non-negative");
        spec.golden = std::move(golden);
    } else if (goldenIt != document.end()) {
        throw std::invalid_argument(
            "smoke mode must not contain a 'golden' object");
    }

    return spec;
}

RenderTestSpec loadRenderTestSpec(const std::filesystem::path &path) {
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (error)
        throw std::invalid_argument("could not read render test spec: " +
                                    path.u8string());
    if (size == 0 || size > kMaximumSpecBytes)
        throw std::invalid_argument(
            "render test spec size is outside the supported range");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::invalid_argument("could not open render test spec: " +
                                    path.u8string());
    nlohmann::json document;
    try {
        input >> document;
    } catch (const nlohmann::json::exception &error) {
        throw std::invalid_argument(std::string("invalid render test JSON: ") +
                                    error.what());
    }
    return parseRenderTestSpec(document, path);
}

nlohmann::json renderTestSpecToJson(const RenderTestSpec &spec) {
    nlohmann::json result = {
        {"schemaVersion", RenderTestSpec::kSchemaVersion},
        {"name", spec.name},
        {"sceneId", spec.sceneId},
        {"profileId", spec.profileId},
        {"shader", spec.shader},
        {"camera",
         {{"position", spec.camera.position},
          {"yaw", spec.camera.yaw},
          {"pitch", spec.camera.pitch}}},
        {"viewport", spec.viewport},
        {"fixedDelta", spec.fixedDelta},
        {"stableFrames", spec.stableFrames},
        {"includeGui", spec.includeGui},
        {"mode", renderTestModeName(spec.mode)},
        {"thresholds",
         {{"minimumNonBlackRatio",
           spec.smokeThresholds.minimumNonBlackRatio},
          {"maximumSolidColorRatio",
           spec.smokeThresholds.maximumSolidColorRatio}}}};
    if (spec.golden) {
        result["golden"] = {
            {"baselineImage", spec.golden->baselineImage.u8string()},
            {"baselineMetadata", spec.golden->baselineMetadata.u8string()},
            {"perChannelAbsoluteThreshold",
             spec.golden->thresholds.perChannelAbsoluteThreshold},
            {"maximumMae", spec.golden->thresholds.maximumMae},
            {"maximumRmse", spec.golden->thresholds.maximumRmse},
            {"maximumBadPixelRatio",
             spec.golden->thresholds.maximumBadPixelRatio}};
    }
    return result;
}

} // namespace vkr::render_test
