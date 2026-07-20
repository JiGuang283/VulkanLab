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

RuntimeCommandError::RuntimeCommandError(std::string code,
                                         std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

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
        } else if (command.method == "render.status") {
            result = host.runtimeRenderStatus();
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
