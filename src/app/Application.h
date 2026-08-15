#pragma once

#include "Config.h"

#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "render/RenderSettings.h"
#include "render/ShaderVariant.h"
#include "workflows/SceneWorkflowTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vkr {

struct CatalogEnvironment;

class Application final {
  public:
    Application(const Config &config, ProjectContext projectContext,
                SceneCatalog catalog);
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void run();

  private:
    void init();
    void initPlatformAndRenderer();
    void initSceneRuntime();
    void initOptionalTooling();
    void shutdown() noexcept;
    void mainLoop();

    void updateInputMode();
    void processCameraInput(float dt);
    void handleSwapChainRecreate();
    const ShaderVariant &currentShaderVariant() const;
    bool hasUnsavedSceneChanges() const;

    uint64_t reloadCurrentScene();
    uint64_t requestSceneLoad(int index, bool sourceFallback = false,
                              bool reloadAsset = false);
    uint64_t requestSceneOperation(int index, bool sourceFallback = false,
                                   bool loadAfter = true,
                                   SceneWorkflowRequestReason reason =
                                       SceneWorkflowRequestReason::SceneLoad,
                                   bool forceReimport = false,
                                   bool reloadAsset = false);
    bool cancelSceneLoad(uint64_t taskId);
    bool cancelLoadOperation(uint64_t taskId);
    bool cancelEnvironmentLoad(uint64_t taskId);
    uint64_t setTextureLimit(uint32_t limit);
    uint64_t setEnvironment(const std::string &id);
    uint64_t queueEnvironmentLoad(const CatalogEnvironment &environment,
                                  bool reload = false);
    uint64_t reloadCurrentEnvironment();
    void applyRenderSettings(const RenderSettingsPatch &patch);
    const RenderSettings &renderSettings() const;
    const CatalogEnvironment *
    findEnvironmentByName(const std::string &name) const;

    struct PlatformServices;
    struct RuntimeServices;
    struct OptionalTooling;
    struct FrameState;

    Config config_;
    ProjectContext projectContext_;
    std::unique_ptr<PlatformServices> platform_;
    std::unique_ptr<RuntimeServices> runtime_;
    std::unique_ptr<OptionalTooling> tooling_;
    std::unique_ptr<FrameState> frame_;
    bool shutdown_ = false;
};

} // namespace vkr
