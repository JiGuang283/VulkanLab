#pragma once

#include "RuntimeCommandDispatcher.h"
#include "workflows/SceneWorkflowTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

class Camera;
class CaptureService;
class Device;
class FrameSync;
class MaterialSystem;
class NamedPipeServerWin32;
class RenderSettingsController;
class Renderer;
class SceneRuntimeCoordinator;
class SceneWorkflowController;
class SwapChain;
class VulkanContext;
class Window;
struct CatalogEnvironment;
struct Config;
struct ProjectContext;
struct RenderSettings;
struct RenderSettingsPatch;
struct RenderViewLightStats;
class SceneCatalog;
struct SceneEntry;
struct SceneLoadContext;
struct ViewMode;
struct VisibilityFrame;

struct RuntimeViewportSnapshot {
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    bool visible = true;
    bool hovered = false;
    bool resizePending = false;
};

struct RuntimeControlActions {
    std::function<uint64_t(int, bool, bool, SceneWorkflowRequestReason,
                           bool, bool)>
        requestSceneOperation;
    std::function<uint64_t(uint32_t)> setTextureLimit;
    std::function<uint64_t(const std::string &)> setEnvironment;
    std::function<uint64_t()> reloadEnvironment;
    std::function<bool(uint64_t)> cancelLoadOperation;
    std::function<bool()> hasUnsavedChanges;
};

struct RuntimeControlServices {
    const Config &config;
    const ProjectContext &projectContext;
    const SceneCatalog &catalog;
    const std::vector<SceneEntry> &sceneRegistry;
    const SceneLoadContext &sceneLoadContext;
    Window &window;
    VulkanContext &context;
    Device &device;
    FrameSync &frameSync;
    SwapChain &swapChain;
    Renderer &renderer;
    MaterialSystem &materialSystem;
    SceneWorkflowController &sceneWorkflow;
    SceneRuntimeCoordinator &sceneRuntime;
    RenderSettingsController &renderSettings;
    CaptureService *captureService = nullptr;
    Camera &camera;
    const VisibilityFrame &visibilityFrame;
    const RenderViewLightStats &lightStats;
    const uint64_t &presentedFrameCount;
    bool editorAvailable = false;
    std::function<RuntimeViewportSnapshot()> viewportSnapshot;
    RuntimeControlActions actions;
};

class RuntimeControlAdapter final : public RuntimeControlHost {
  public:
    explicit RuntimeControlAdapter(RuntimeControlServices services);
    ~RuntimeControlAdapter() override;

    RuntimeControlAdapter(const RuntimeControlAdapter &) = delete;
    RuntimeControlAdapter &operator=(const RuntimeControlAdapter &) = delete;

    bool start();
    void stop();
    bool processOne();

  private:
    ControlJson runtimeSystemInfo() override;
    ControlJson runtimeSceneList() override;
    ControlJson runtimeSceneCurrent() override;
    ControlJson runtimeSceneLoad(const std::string &name) override;
    ControlJson runtimeSceneReload() override;
    ControlJson runtimeLoadStatus(std::optional<uint64_t> taskId) override;
    ControlJson runtimeLoadCancel(std::optional<uint64_t> taskId) override;
    ControlJson runtimeTextureLimitGet() override;
    ControlJson runtimeTextureLimitSet(uint32_t value) override;
    ControlJson runtimeAssetCatalog() override;
    ControlJson runtimeAssetStatus(const std::optional<std::string> &name) override;
    ControlJson runtimeAssetValidation(const std::string &name) override;
    ControlJson runtimeAssetImport(const std::string &name, bool force,
                                   bool loadAfter) override;
    ControlJson runtimeAssetCancel(std::optional<uint64_t> taskId) override;
    ControlJson runtimeAssetCacheInfo() override;
    ControlJson runtimeShaderList() override;
    ControlJson runtimeShaderCurrent() override;
    ControlJson runtimeShaderSet(const std::string &name) override;
    ControlJson runtimeCameraGet() override;
    ControlJson runtimeCameraSet(const RuntimeCameraPose &pose) override;
    ControlJson runtimeWindowResize(uint32_t width, uint32_t height) override;
    ControlJson runtimeRenderStatus() override;
    ControlJson runtimeRenderPathGet() override;
    ControlJson runtimeRenderPathSet(RenderPathRequest request) override;
    ControlJson runtimeRenderSettingsGet() override;
    ControlJson runtimeRenderSettingsSet(const RenderSettingsPatch &patch) override;
    ControlJson runtimeEnvironmentList() override;
    ControlJson runtimeEnvironmentCurrent() override;
    ControlJson runtimeEnvironmentSet(const std::string &name) override;
    ControlJson runtimeEnvironmentReload() override;
    ControlJson runtimeCaptureScreenshot(const std::string &path,
                                         bool includeGui) override;
    ControlJson runtimeCaptureStatus(uint64_t taskId) override;
    ControlJson runtimeCaptureCancel(uint64_t taskId) override;
    ControlJson runtimeLastLoadStats() override;
    ControlJson runtimeQuit() override;

    ControlJson runtimeSceneOperationResult(int index, uint64_t taskId);
    int runtimeAssetSceneIndex(const std::string &name) const;
    ControlJson runtimeIndexedArtifactStatus(int index) const;

    uint64_t requestSceneOperation(
        int index, bool sourceFallback = false, bool loadAfter = true,
        SceneWorkflowRequestReason reason =
            SceneWorkflowRequestReason::SceneLoad,
        bool forceReimport = false, bool reloadAsset = false);
    uint64_t setTextureLimit(uint32_t limit);
    uint64_t setEnvironment(const std::string &id);
    uint64_t reloadCurrentEnvironment();
    bool cancelLoadOperation(uint64_t taskId);
    bool hasUnsavedSceneChanges() const;
    void setViewMode(const std::string &id);
    void applyRenderSettings(const RenderSettingsPatch &patch);
    const RenderSettings &renderSettings() const;
    const ViewMode &currentViewMode() const;
    int findSceneIndexByName(const std::string &name) const;
    const CatalogEnvironment *
    findEnvironmentByName(const std::string &name) const;
    std::string profileIdForTextureLimit(const SceneEntry &entry) const;

    const Config &config_;
    const ProjectContext &projectContext_;
    const SceneCatalog &catalog_;
    const std::vector<SceneEntry> &sceneRegistry_;
    const SceneLoadContext &sceneLoadContext_;
    Window *window_ = nullptr;
    VulkanContext *context_ = nullptr;
    Device *device_ = nullptr;
    FrameSync *frameSync_ = nullptr;
    SwapChain *swapChain_ = nullptr;
    Renderer *renderer_ = nullptr;
    MaterialSystem *materialSystem_ = nullptr;
    SceneWorkflowController *sceneWorkflow_ = nullptr;
    SceneRuntimeCoordinator *sceneRuntime_ = nullptr;
    RenderSettingsController *renderSettingsController_ = nullptr;
    CaptureService *captureService_ = nullptr;
    Camera &camera_;
    const VisibilityFrame &visibilityFrame_;
    const RenderViewLightStats &lastLightStats_;
    const uint64_t &presentedFrameCount_;
    bool gui_ = false;
    std::function<RuntimeViewportSnapshot()> viewportSnapshot_;
    RuntimeControlActions actions_;
    std::string runtimeControlPipeName_;

    std::unique_ptr<RuntimeCommandQueue> runtimeCommandQueue_;
    std::unique_ptr<NamedPipeServerWin32> runtimeControlServer_;
    RuntimeCommandDispatcher runtimeCommandDispatcher_;
    std::shared_ptr<RuntimeCommand> pendingQuitCommand_;
};

} // namespace vkr
