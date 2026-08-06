#include "RuntimeCommandDispatcher.h"

#include "core/Log.h"

#include <cmath>
#include <limits>
#include <utility>

namespace vkr {
namespace {

std::string requiredString(const RuntimeCommand &command, const char *name) {
    if (!command.params.contains(name) ||
        !command.params[name].is_string()) {
        throw RuntimeCommandError(
            "invalid_params",
            std::string("Parameter '") + name + "' must be a string.");
    }
    return command.params[name].get<std::string>();
}

std::optional<std::string> optionalString(const RuntimeCommand &command,
                                          const char *name) {
    if (!command.params.contains(name))
        return std::nullopt;
    if (!command.params[name].is_string()) {
        throw RuntimeCommandError(
            "invalid_params",
            std::string("Parameter '") + name + "' must be a string.");
    }
    return command.params[name].get<std::string>();
}

std::optional<uint64_t> optionalUnsigned(const RuntimeCommand &command,
                                         const char *name) {
    if (!command.params.contains(name))
        return std::nullopt;
    if (!command.params[name].is_number_unsigned()) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be an unsigned integer.");
    }
    return command.params[name].get<uint64_t>();
}

uint32_t requiredUint32(const RuntimeCommand &command, const char *name) {
    const std::optional<uint64_t> value = optionalUnsigned(command, name);
    if (!value || *value > std::numeric_limits<uint32_t>::max()) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be an unsigned 32-bit integer.");
    }
    return static_cast<uint32_t>(*value);
}

std::optional<uint32_t> optionalUint32(const RuntimeCommand &command,
                                       const char *name,
                                       uint32_t maximum) {
    if (!command.params.contains(name))
        return std::nullopt;
    const uint32_t value = requiredUint32(command, name);
    if (value > maximum) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' is outside the supported range.");
    }
    return value;
}

uint32_t requiredWindowDimension(const RuntimeCommand &command,
                                 const char *name) {
    const uint32_t value = requiredUint32(command, name);
    if (value < 1 || value > 16384) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be in 1..16384.");
    }
    return value;
}

uint64_t requiredUint64(const RuntimeCommand &command, const char *name) {
    const std::optional<uint64_t> value = optionalUnsigned(command, name);
    if (!value) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be an unsigned integer.");
    }
    return *value;
}

bool optionalBool(const RuntimeCommand &command, const char *name,
                  bool fallback) {
    if (!command.params.contains(name))
        return fallback;
    if (!command.params[name].is_boolean()) {
        throw RuntimeCommandError(
            "invalid_params",
            std::string("Parameter '") + name + "' must be a boolean.");
    }
    return command.params[name].get<bool>();
}

float requiredFiniteFloat(const RuntimeCommand &command, const char *name);

std::optional<bool> optionalBoolValue(const RuntimeCommand &command,
                                      const char *name) {
    if (!command.params.contains(name))
        return std::nullopt;
    if (!command.params[name].is_boolean()) {
        throw RuntimeCommandError(
            "invalid_params",
            std::string("Parameter '") + name + "' must be a boolean.");
    }
    return command.params[name].get<bool>();
}

std::optional<float> optionalFiniteFloat(const RuntimeCommand &command,
                                         const char *name, float minimum,
                                         float maximum) {
    if (!command.params.contains(name))
        return std::nullopt;
    const float value = requiredFiniteFloat(command, name);
    if (value < minimum || value > maximum) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' is outside the supported range.");
    }
    return value;
}

RenderSettingsPatch renderSettingsPatch(const RuntimeCommand &command) {
    RenderSettingsPatch patch;
    patch.shadowsEnabled = optionalBoolValue(command, "shadowsEnabled");
    patch.shadowReceiverBias = optionalFiniteFloat(
        command, "shadowReceiverBias", 0.0f, 0.05f);
    patch.shadowConstantBias = optionalFiniteFloat(
        command, "shadowConstantBias", 0.0f, 10.0f);
    patch.shadowSlopeBias = optionalFiniteFloat(
        command, "shadowSlopeBias", 0.0f, 10.0f);
    patch.exposureEv =
        optionalFiniteFloat(command, "exposureEv", -10.0f, 10.0f);
    patch.iblEnabled = optionalBoolValue(command, "iblEnabled");
    patch.skyboxEnabled = optionalBoolValue(command, "skyboxEnabled");
    patch.environmentIntensity = optionalFiniteFloat(
        command, "environmentIntensity", 0.0f, 100.0f);
    patch.environmentRotationRadians = optionalFiniteFloat(
        command, "environmentRotationRadians", -1000.0f, 1000.0f);
    patch.bloomEnabled = optionalBoolValue(command, "bloomEnabled");
    patch.bloomThreshold = optionalFiniteFloat(
        command, "bloomThreshold", 0.0f, 20.0f);
    patch.bloomSoftKnee = optionalFiniteFloat(
        command, "bloomSoftKnee", 0.0f, 1.0f);
    patch.bloomIntensity = optionalFiniteFloat(
        command, "bloomIntensity", 0.0f, 5.0f);
    patch.frustumCullingEnabled =
        optionalBoolValue(command, "frustumCullingEnabled");
    patch.shadowCullingEnabled =
        optionalBoolValue(command, "shadowCullingEnabled");
    patch.shadowDistance = optionalFiniteFloat(
        command, "shadowDistance", 0.1f, 100000.0f);
    patch.distanceCullingEnabled =
        optionalBoolValue(command, "distanceCullingEnabled");
    patch.maxDrawDistance = optionalFiniteFloat(
        command, "maxDrawDistance", 0.1f, 1000000.0f);
    patch.smallObjectCullingEnabled =
        optionalBoolValue(command, "smallObjectCullingEnabled");
    patch.minProjectedSizePixels = optionalFiniteFloat(
        command, "minProjectedSizePixels", 0.0f, 256.0f);
    patch.occlusionCullingEnabled =
        optionalBoolValue(command, "occlusionCullingEnabled");
    patch.occlusionDepthBias = optionalFiniteFloat(
        command, "occlusionDepthBias", 0.0f, 0.05f);
    patch.surfaceMotionDebugScale = optionalFiniteFloat(
        command, "surfaceMotionDebugScale", 0.1f, 1024.0f);
    patch.ssaoRadius = optionalFiniteFloat(
        command, "ssaoRadius", 0.05f, 10.0f);
    patch.ssaoBias = optionalFiniteFloat(
        command, "ssaoBias", 0.0f, 0.2f);
    patch.ssaoIntensity = optionalFiniteFloat(
        command, "ssaoIntensity", 0.0f, 4.0f);
    patch.ssaoPower = optionalFiniteFloat(
        command, "ssaoPower", 0.25f, 4.0f);
    patch.cacaoRadius = optionalFiniteFloat(
        command, "cacaoRadius", 0.05f, 10.0f);
    patch.cacaoIntensity = optionalFiniteFloat(
        command, "cacaoIntensity", 0.0f, 4.0f);
    patch.cacaoPower = optionalFiniteFloat(
        command, "cacaoPower", 0.25f, 4.0f);
    patch.taaHistoryWeight = optionalFiniteFloat(
        command, "taaHistoryWeight", 0.0f, 0.99f);
    patch.taaSharpness = optionalFiniteFloat(
        command, "taaSharpness", 0.0f, 1.0f);
    patch.screenSpaceDebugMip =
        optionalUint32(command, "screenSpaceDebugMip", 31u);
    if (const auto toneMapper = optionalString(command, "toneMapper")) {
        patch.toneMapper = toneMapperFromName(*toneMapper);
        if (!patch.toneMapper) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'toneMapper' must be passthrough, reinhard, or aces.");
        }
    }
    if (const auto debugView =
            optionalString(command, "surfaceDebugView")) {
        patch.surfaceDebugView = surfaceDebugViewFromName(*debugView);
        if (!patch.surfaceDebugView) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'surfaceDebugView' must be none, normal, "
                "roughness, motion, or history-validity.");
        }
    }
    if (const auto aoMode =
            optionalString(command, "ambientOcclusionMode")) {
        patch.ambientOcclusionMode = ambientOcclusionModeFromName(*aoMode);
        if (!patch.ambientOcclusionMode) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'ambientOcclusionMode' must be off, ssao, or cacao.");
        }
    }
    if (const auto quality = optionalString(command, "ssaoQuality")) {
        patch.ssaoQuality = ssaoQualityFromName(*quality);
        if (!patch.ssaoQuality) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'ssaoQuality' must be low, medium, or high.");
        }
    }
    if (const auto quality = optionalString(command, "cacaoQuality")) {
        patch.cacaoQuality = cacaoQualityFromName(*quality);
        if (!patch.cacaoQuality) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'cacaoQuality' must be lowest, low, medium, high, "
                "or highest.");
        }
    }
    if (const auto resolution = optionalString(command, "cacaoResolution")) {
        patch.cacaoResolution = cacaoResolutionFromName(*resolution);
        if (!patch.cacaoResolution) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'cacaoResolution' must be native or half.");
        }
    }
    if (const auto mode = optionalString(command, "temporalAntiAliasingMode")) {
        patch.temporalAntiAliasingMode =
            temporalAntiAliasingModeFromName(*mode);
        if (!patch.temporalAntiAliasingMode) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'temporalAntiAliasingMode' must be off or taa.");
        }
    }
    if (const auto debugView =
            optionalString(command, "screenSpaceDebugView")) {
        patch.screenSpaceDebugView = screenSpaceDebugViewFromName(*debugView);
        if (!patch.screenSpaceDebugView) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'screenSpaceDebugView' must be none, "
                "nearest-depth, scene-color, ssao-raw, ssao-filtered, or "
                "cacao-output, taa-history, taa-rejection, or "
                "taa-history-weight.");
        }
    }
    if (!patch.shadowsEnabled && !patch.shadowReceiverBias &&
        !patch.shadowConstantBias && !patch.shadowSlopeBias &&
        !patch.exposureEv && !patch.toneMapper && !patch.iblEnabled &&
        !patch.skyboxEnabled && !patch.environmentIntensity &&
        !patch.environmentRotationRadians && !patch.bloomEnabled &&
        !patch.bloomThreshold && !patch.bloomSoftKnee &&
        !patch.bloomIntensity && !patch.frustumCullingEnabled &&
        !patch.shadowCullingEnabled && !patch.shadowDistance &&
        !patch.distanceCullingEnabled && !patch.maxDrawDistance &&
        !patch.smallObjectCullingEnabled &&
        !patch.minProjectedSizePixels && !patch.occlusionCullingEnabled &&
        !patch.occlusionDepthBias && !patch.surfaceDebugView &&
        !patch.surfaceMotionDebugScale && !patch.ambientOcclusionMode &&
        !patch.ssaoQuality && !patch.ssaoRadius && !patch.ssaoBias &&
        !patch.ssaoIntensity && !patch.ssaoPower && !patch.cacaoQuality &&
        !patch.cacaoResolution && !patch.cacaoRadius &&
        !patch.cacaoIntensity && !patch.cacaoPower &&
        !patch.temporalAntiAliasingMode && !patch.taaHistoryWeight &&
        !patch.taaSharpness &&
        !patch.screenSpaceDebugView && !patch.screenSpaceDebugMip) {
        throw RuntimeCommandError(
            "invalid_params",
            "render_settings.set requires at least one setting.");
    }
    return patch;
}

float requiredFiniteFloat(const RuntimeCommand &command, const char *name) {
    if (!command.params.contains(name) ||
        !command.params[name].is_number()) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be a finite number.");
    }
    const double value = command.params[name].get<double>();
    if (!std::isfinite(value) ||
        value < -std::numeric_limits<float>::max() ||
        value > std::numeric_limits<float>::max()) {
        throw RuntimeCommandError(
            "invalid_params", std::string("Parameter '") + name +
                                  "' must be a finite number.");
    }
    return static_cast<float>(value);
}

RuntimeCameraPose requiredCameraPose(const RuntimeCommand &command) {
    if (!command.params.contains("position") ||
        !command.params["position"].is_array() ||
        command.params["position"].size() != 3) {
        throw RuntimeCommandError(
            "invalid_params",
            "Parameter 'position' must be an array of three finite numbers.");
    }

    RuntimeCameraPose pose;
    for (size_t index = 0; index < pose.position.size(); ++index) {
        const ControlJson &component = command.params["position"][index];
        if (!component.is_number()) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'position' must be an array of three finite numbers.");
        }
        const double value = component.get<double>();
        if (!std::isfinite(value) ||
            value < -std::numeric_limits<float>::max() ||
            value > std::numeric_limits<float>::max()) {
            throw RuntimeCommandError(
                "invalid_params",
                "Parameter 'position' must be an array of three finite numbers.");
        }
        pose.position[index] = static_cast<float>(value);
    }
    pose.yaw = requiredFiniteFloat(command, "yaw");
    pose.pitch = requiredFiniteFloat(command, "pitch");
    return pose;
}

} // namespace

RuntimeDispatchResult RuntimeCommandDispatcher::dispatch(
    const RuntimeCommand &command, RuntimeControlHost &host) const {
    RuntimeDispatchResult dispatched;
    try {
        ControlJson result = ControlJson::object();
        if (command.method == "system.ping") {
            result = {{"message", "pong"}};
        } else if (command.method == "system.info") {
            result = host.runtimeSystemInfo();
        } else if (command.method == "scene.list") {
            result = host.runtimeSceneList();
        } else if (command.method == "scene.current") {
            result = host.runtimeSceneCurrent();
        } else if (command.method == "scene.load") {
            result = host.runtimeSceneLoad(requiredString(command, "name"));
        } else if (command.method == "scene.reload") {
            result = host.runtimeSceneReload();
        } else if (command.method == "load.status") {
            result = host.runtimeLoadStatus(
                optionalUnsigned(command, "taskId"));
        } else if (command.method == "load.cancel") {
            result = host.runtimeLoadCancel(
                optionalUnsigned(command, "taskId"));
        } else if (command.method == "texture_limit.get") {
            result = host.runtimeTextureLimitGet();
        } else if (command.method == "texture_limit.set") {
            result = host.runtimeTextureLimitSet(
                requiredUint32(command, "value"));
        } else if (command.method == "asset.catalog") {
            result = host.runtimeAssetCatalog();
        } else if (command.method == "asset.status") {
            result = host.runtimeAssetStatus(optionalString(command, "name"));
        } else if (command.method == "asset.validation") {
            result = host.runtimeAssetValidation(
                requiredString(command, "name"));
        } else if (command.method == "asset.import") {
            result = host.runtimeAssetImport(
                requiredString(command, "name"),
                optionalBool(command, "force", false),
                optionalBool(command, "loadAfter", false));
        } else if (command.method == "asset.cancel") {
            result = host.runtimeAssetCancel(
                optionalUnsigned(command, "taskId"));
        } else if (command.method == "asset.cache_info") {
            result = host.runtimeAssetCacheInfo();
        } else if (command.method == "shader.list") {
            result = host.runtimeShaderList();
        } else if (command.method == "shader.current") {
            result = host.runtimeShaderCurrent();
        } else if (command.method == "shader.set") {
            result = host.runtimeShaderSet(requiredString(command, "name"));
        } else if (command.method == "camera.get") {
            result = host.runtimeCameraGet();
        } else if (command.method == "camera.set") {
            result = host.runtimeCameraSet(requiredCameraPose(command));
        } else if (command.method == "window.resize") {
            result = host.runtimeWindowResize(
                requiredWindowDimension(command, "width"),
                requiredWindowDimension(command, "height"));
        } else if (command.method == "render.status") {
            result = host.runtimeRenderStatus();
        } else if (command.method == "render_settings.get") {
            result = host.runtimeRenderSettingsGet();
        } else if (command.method == "render_settings.set") {
            result = host.runtimeRenderSettingsSet(
                renderSettingsPatch(command));
        } else if (command.method == "environment.list") {
            result = host.runtimeEnvironmentList();
        } else if (command.method == "environment.current") {
            result = host.runtimeEnvironmentCurrent();
        } else if (command.method == "environment.set") {
            result = host.runtimeEnvironmentSet(
                requiredString(command, "name"));
        } else if (command.method == "environment.reload") {
            result = host.runtimeEnvironmentReload();
        } else if (command.method == "capture.screenshot") {
            result = host.runtimeCaptureScreenshot(
                requiredString(command, "path"),
                optionalBool(command, "includeGui", false));
        } else if (command.method == "capture.status") {
            result = host.runtimeCaptureStatus(
                requiredUint64(command, "taskId"));
        } else if (command.method == "capture.cancel") {
            result = host.runtimeCaptureCancel(
                requiredUint64(command, "taskId"));
        } else if (command.method == "stats.last_load") {
            result = host.runtimeLastLoadStats();
        } else if (command.method == "app.quit") {
            result = host.runtimeQuit();
            dispatched.requestQuit = true;
        } else {
            throw RuntimeCommandError("method_not_found",
                                      "Unknown method '" + command.method +
                                          "'.");
        }
        dispatched.response =
            makeRuntimeSuccess(command.id, std::move(result));
    } catch (const RuntimeCommandError &error) {
        VKR_LOG_WARN("Control", "Command {} failed: {}", command.method,
                     error.what());
        dispatched.response =
            makeRuntimeError(command.id, error.code(), error.what());
    } catch (const std::exception &error) {
        VKR_LOG_ERROR("Control", "Command {} failed: {}", command.method,
                      error.what());
        dispatched.response = makeRuntimeError(
            command.id, "command_failed", error.what());
    }
    return dispatched;
}

} // namespace vkr
