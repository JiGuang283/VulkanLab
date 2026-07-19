#pragma once

#include "RuntimeCommand.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace vkr {

class RuntimeCommandError : public std::runtime_error {
  public:
    RuntimeCommandError(std::string code, std::string message);

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
    virtual ControlJson runtimeAssetImport(const std::string &name,
                                           bool force,
                                           bool loadAfter) = 0;
    virtual ControlJson
    runtimeAssetCancel(std::optional<uint64_t> taskId) = 0;
    virtual ControlJson runtimeAssetCacheInfo() = 0;
    virtual ControlJson runtimeShaderList() = 0;
    virtual ControlJson runtimeShaderCurrent() = 0;
    virtual ControlJson runtimeShaderSet(const std::string &name) = 0;
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
