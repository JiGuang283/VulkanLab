#pragma once

#include "render/frame/RenderFeatureState.h"
#include "render/frame/RenderSettings.h"
#include "render/shader/ShaderRegistry.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace vkr {

class RenderSettingsError : public std::runtime_error {
  public:
    RenderSettingsError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

  private:
    std::string code_;
};

struct RenderSettingsCallbacks {
    std::function<void(CacaoResolution)> reconfigureCacao;
};

struct RenderSettingsSnapshot {
    RenderSettings requested{};
    RenderSettings active{};
    RenderFeatureSupport support{};
    RenderFeatureRuntimeState runtime{};
    RenderPathSelection renderPath{};
    std::string viewModeId;
    std::string viewModeDisplayName;
};

class RenderSettingsController {
  public:
    explicit RenderSettingsController(const std::filesystem::path &manifestPath);

    const ShaderRegistry &shaderRegistry() const { return shaderRegistry_; }
    const ViewMode &currentViewMode() const;
    const RenderSettings &settings() const { return settings_; }
    RenderPathSelection renderPathSelection() const;

    void setViewMode(const std::string &id);
    void apply(const RenderSettingsPatch &patch);

    void configure(RenderFeatureSupport support,
                   RenderSettingsCallbacks callbacks = {});
    void updateRuntimeState(RenderFeatureRuntimeState state);
    RenderSettingsSnapshot snapshot() const;

  private:
    void validatePatch(const RenderSettingsPatch &patch) const;
    static void normalize(RenderSettings &settings);
    RenderSettings activeSettings() const;

    ShaderRegistry shaderRegistry_;
    std::string currentViewModeId_;
    RenderSettings settings_{};
    RenderFeatureSupport support_{};
    RenderFeatureRuntimeState runtime_{};
    RenderSettingsCallbacks callbacks_{};
    bool configured_ = false;
};

} // namespace vkr
