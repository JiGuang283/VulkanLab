#pragma once

#include "RuntimeCommand.h"
#include "render/frame/RenderSettings.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace vkr {

struct RuntimeCameraPose {
    std::array<float, 3> position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

class RuntimeCommandError : public std::runtime_error {
  public:
    RuntimeCommandError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const { return code_; }

  private:
    std::string code_;
};

class RuntimeControlHost {
  public:
    virtual ~RuntimeControlHost() = default;

    virtual ControlJson runtimeSystemInfo() = 0;
    virtual ControlJson runtimeSceneList() = 0;
    virtual ControlJson runtimeSceneCurrent() = 0;
    virtual ControlJson runtimeSceneLoad(const std::string &name) = 0;
    virtual ControlJson runtimeSceneReload() = 0;
    virtual ControlJson
    runtimeLoadStatus(std::optional<uint64_t> taskId) = 0;
    virtual ControlJson
    runtimeLoadCancel(std::optional<uint64_t> taskId) = 0;
    virtual ControlJson runtimeTextureLimitGet() = 0;
    virtual ControlJson runtimeTextureLimitSet(uint32_t value) = 0;
    virtual ControlJson runtimeAssetCatalog() = 0;
    virtual ControlJson
    runtimeAssetStatus(const std::optional<std::string> &name) = 0;
    virtual ControlJson runtimeAssetValidation(const std::string &name) = 0;
    virtual ControlJson runtimeAssetImport(const std::string &name,
                                           bool force,
                                           bool loadAfter) = 0;
    virtual ControlJson
    runtimeAssetCancel(std::optional<uint64_t> taskId) = 0;
    virtual ControlJson runtimeAssetCacheInfo() = 0;
    virtual ControlJson runtimeShaderList() = 0;
    virtual ControlJson runtimeShaderCurrent() = 0;
    virtual ControlJson runtimeShaderSet(const std::string &name) = 0;
    virtual ControlJson runtimeCameraGet() = 0;
    virtual ControlJson runtimeCameraSet(const RuntimeCameraPose &pose) = 0;
    virtual ControlJson runtimeWindowResize(uint32_t width,
                                            uint32_t height) = 0;
    virtual ControlJson runtimeRenderStatus() = 0;
    virtual ControlJson runtimeRenderPathGet() = 0;
    virtual ControlJson runtimeRenderPathSet(RenderPathRequest request) = 0;
    virtual ControlJson runtimeRenderSettingsGet() = 0;
    virtual ControlJson
    runtimeRenderSettingsSet(const RenderSettingsPatch &patch) = 0;
    virtual ControlJson runtimeEnvironmentList() = 0;
    virtual ControlJson runtimeEnvironmentCurrent() = 0;
    virtual ControlJson
    runtimeEnvironmentSet(const std::string &name) = 0;
    virtual ControlJson runtimeEnvironmentReload() = 0;
    virtual ControlJson runtimeCaptureScreenshot(const std::string &path,
                                                 bool includeGui) = 0;
    virtual ControlJson runtimeCaptureStatus(uint64_t taskId) = 0;
    virtual ControlJson runtimeCaptureCancel(uint64_t taskId) = 0;
    virtual ControlJson runtimeLastLoadStats() = 0;
    virtual ControlJson runtimeQuit() = 0;
};

struct RuntimeDispatchResult {
    ControlJson response;
    bool requestQuit = false;
};

class RuntimeCommandDispatcher {
  public:
    RuntimeDispatchResult dispatch(const RuntimeCommand &command,
                                   RuntimeControlHost &host) const;
};

} // namespace vkr
