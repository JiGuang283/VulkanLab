#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "render/RenderGraph.h"
#include "render/RenderFrame.h"
#include "render/RenderFeatureState.h"
#include "render/FrameGpuData.h"
#include "render/GpuPassProfiler.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderSettings.h"
#include "render/RendererProgramCatalog.h"
#include "render/Atmosphere.h"
#include "render/Visibility.h"
#include "scene_data/SceneIds.h"

#include <memory>
#include <array>
#include <deque>
#include <functional>
#include <string>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class MaterialSystem;
class ShaderRegistry;
struct EnvironmentGpuResources;
class MainForwardPass;
class SurfacePrepass;
class AtmosphereLutPass;
class ToneMapPass;
class CacaoPass;
class GtaoPass;
class SsrPass;
class SsgiPass;
class TaaPass;
class DdgiPass;
struct DdgiRuntimeStatus;
class Texture;
class PresentPass;
class PipelineCache;
class RayTracingScene;
struct RayTracingSceneStatus;
struct ShaderVariant;
struct RenderView;

struct RendererViewportOutput {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<VkImage, MAX_FRAMES_IN_FLIGHT> images{};
    std::array<VkImageView, MAX_FRAMES_IN_FLIGHT> imageViews{};
};

struct RendererHdrOutput {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::array<VkImage, MAX_FRAMES_IN_FLIGHT> images{};
};

struct SceneLightBufferStatus {
    uint32_t limit = kMaxSceneLights;
    uint32_t activeLights = 0;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> frameCapacities{};
    uint64_t allocatedBytes = 0;
};

struct ReflectionProbeRuntimeStatus {
    uint32_t limit = kMaxReflectionProbes;
    uint32_t sourceCount = 0;
    uint32_t activeCount = 0;
    uint32_t ignoredCount = 0;
    uint64_t descriptorGeneration = 0;
    uint64_t allocatedBytes = 0;
    std::vector<PersistentEntityId> ignoredEntityIds;
};

struct OcclusionCullingStatus {
    bool supported = false;
    bool active = false;
    std::string unavailableReason;
    uint32_t hiZMipLevels = 0;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> indirectCapacities{};
    uint64_t allocatedBytes = 0;
    uint32_t latestCandidates = 0;
    uint32_t latestUncullable = 0;
    CompletedGpuVisibilityStatistics completed{};
};

struct SurfaceDataStatus {
    bool supported = false;
    bool active = false;
    std::string unavailableReason;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat normalRoughnessFormat = VK_FORMAT_UNDEFINED;
    VkFormat motionFormat = VK_FORMAT_UNDEFINED;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> historyCapacities{};
    uint64_t allocatedBytes = 0;
};

struct ScreenSpaceEffectsStatus {
    bool depthPyramidSupported = false;
    bool colorPyramidSupported = false;
    bool ssaoSupported = false;
    bool cacaoCompiled = false;
    bool cacaoSupported = false;
    bool cacaoInitialized = false;
    bool cacaoFp32 = true;
    bool cacaoInternalMemoryTracked = false;
    bool gtaoSupported = false;
    bool gtaoActive = false;
    bool gtaoHistoryValid = false;
    bool taaSupported = false;
    bool taaActive = false;
    bool taaHistoryValid = false;
    bool ssrSupported = false;
    bool ssrActive = false;
    bool ssrHistoryValid = false;
    bool ssgiSupported = false;
    bool ssgiActive = false;
    bool ssgiHistoryValid = false;
    AmbientOcclusionMode requestedMode = AmbientOcclusionMode::Off;
    AmbientOcclusionMode activeMode = AmbientOcclusionMode::Off;
    GlobalIlluminationMode requestedGiMode =
        GlobalIlluminationMode::AmbientOrIbl;
    GlobalIlluminationMode activeGiMode =
        GlobalIlluminationMode::AmbientOrIbl;
    uint32_t depthMipLevels = 0;
    uint32_t colorMipLevels = 0;
    VkExtent2D depthExtent{};
    VkExtent2D colorExtent{};
    VkExtent2D ssaoExtent{};
    VkExtent2D cacaoOutputExtent{};
    CacaoResolution cacaoResolution = CacaoResolution::Half;
    uint64_t cacaoGeneration = 0;
    VkExtent2D gtaoExtent{};
    uint64_t gtaoHistoryGeneration = 0;
    uint64_t gtaoLastFrameSerial = 0;
    VkExtent2D taaExtent{};
    VkExtent2D ssrExtent{};
    VkExtent2D ssgiExtent{};
    uint64_t taaHistoryGeneration = 0;
    uint64_t taaLastFrameSerial = 0;
    uint64_t ssrHistoryGeneration = 0;
    uint64_t ssrLastFrameSerial = 0;
    uint64_t ssgiHistoryGeneration = 0;
    uint64_t ssgiLastFrameSerial = 0;
    glm::vec2 taaJitterPixels{0.0f};
    uint64_t estimatedMemoryBytes = 0;
    std::string depthPyramidUnavailableReason;
    std::string colorPyramidUnavailableReason;
    std::string ssaoUnavailableReason;
    std::string cacaoUnavailableReason;
    std::string gtaoUnavailableReason;
    std::string gtaoLastResetReason;
    std::string taaUnavailableReason;
    std::string taaLastResetReason;
    std::string ssrUnavailableReason;
    std::string ssrLastResetReason;
    std::string ssgiUnavailableReason;
    std::string ssgiLastResetReason;
};

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             DescriptorAllocator &descriptorAllocator,
             MaterialSystem &materialSystem, const ShaderRegistry &shaderRegistry,
             MaterialBindingMode materialBindingMode);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void renderFrame(const FrameSync::FrameContext &frame,
                     const VisibilityFrame &visibility,
                     PipelineCache &pipelineCache,
                     std::function<void(VkCommandBuffer)> drawUi,
                     const ShaderVariant &shaderVariant,
                     const RenderView &view,
                     std::optional<FrameCaptureSource> captureSource = {},
                     std::function<void(VkCommandBuffer)> screenshotCopy = {});

    // ---- 交换链重建 ----
    void recreateSwapChain();
    void resizeViewport(VkExtent2D extent);

    // ---- 访问器 ----
    VkExtent2D viewportExtent() const;
    RendererViewportOutput viewportOutput() const;
    RendererHdrOutput hdrOutput() const;
    const GpuPassTimings &gpuPassTimings() const;
    const RenderGraphDiagnostics &renderGraphDiagnostics() const {
        return renderGraph_.diagnostics();
    }
    std::string renderGraphJson() const { return renderGraph_.toJson(); }
    std::string renderGraphDot() const { return renderGraph_.toDot(); }
    VkDescriptorSetLayout globalDescriptorSetLayout() const {
        return globalDescriptorSetLayout_;
    }
    VkDescriptorSetLayout lightingDescriptorSetLayout() const {
        return lightingDescriptorSetLayout_;
    }
    void publishEnvironment(
        std::shared_ptr<EnvironmentGpuResources> environment);
    void clearEnvironment();
    bool environmentReady() const;
    std::string currentEnvironmentId() const;
    float currentEnvironmentMaxSpecularLod() const;
    bool bloomSupported() const;
    const std::string &bloomUnsupportedReason() const;
    SceneLightBufferStatus sceneLightBufferStatus() const;
    ReflectionProbeRuntimeStatus reflectionProbeStatus() const;
    const RayTracingSceneStatus &rayTracingSceneStatus() const;
    OcclusionCullingStatus occlusionCullingStatus() const;
    SurfaceDataStatus surfaceDataStatus() const;
    ScreenSpaceEffectsStatus screenSpaceEffectsStatus() const;
    bool reconfigureCacao(CacaoResolution resolution, std::string &error);
    bool atmosphereSupported() const;
    const std::string &atmosphereUnsupportedReason() const;
    AtmosphereRuntimeStatus atmosphereStatus() const;
    DdgiRuntimeStatus ddgiStatus() const;
    RenderFeatureSupport featureSupport() const;
    RenderFeatureRuntimeState featureRuntimeState() const {
        return featureRuntimeState_;
    }

    // ---- per-frame UBO 访问器 ----
  private:
    struct FrameSceneLightStorage {
        std::unique_ptr<Buffer> buffer;
        uint32_t capacity = 0;
    };

    struct LightingDescriptorGeneration {
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
        std::shared_ptr<EnvironmentGpuResources> environment;
        std::vector<std::shared_ptr<EnvironmentGpuResources>>
            reflectionProbeEnvironments;
        uint64_t generation = 0;
        uint64_t retireAfterSerial = 0;
    };

    void createUniformBuffers();
    void createSceneLightBuffers();
    void createReflectionProbeBuffers();
    void ensureSceneLightCapacity(uint32_t frameIndex,
                                  uint32_t requiredLights);
    void updateSceneLightDescriptor(uint32_t frameIndex);
    void createGlobalDescriptorSetLayout();
    void createGlobalDescriptorSets();
    void createLightingDescriptorSetLayout();
    void createAtmosphereUniformBuffers();
    void createAtmosphereDescriptorSetLayout();
    void createAtmosphereDescriptorSets();
    void updateAtmosphereDescriptor(uint32_t frameIndex, bool active);
    void createScreenSpaceUniformBuffers();
    void createScreenSpaceDescriptorSetLayout();
    void createScreenSpaceFallback();
    void createScreenSpaceDescriptorSets();
    void updateScreenSpaceDescriptor(uint32_t frameIndex,
                                     AmbientOcclusionMode mode);
    void initializeAtmosphereImages();
    void initializeShadowImages();
    void createFallbackEnvironment();
    void createLightingGeneration(
        std::shared_ptr<EnvironmentGpuResources> environment,
        std::vector<std::shared_ptr<EnvironmentGpuResources>>
            reflectionProbeEnvironments = {});
    void updateReflectionProbes(const RenderView &view,
                                uint32_t frameIndex);
    void collectRetiredLightingGenerations();
    void freeLightingGeneration(
        LightingDescriptorGeneration &generation);
    void createRenderGraph();
    void registerAtmosphereShadowFeatures();
    void registerSurfaceVisibilityFeatures();
    void registerIndirectLightingPreparationFeatures();
    void registerSceneLightingFeatures();
    void registerPostProcessFeatures();
    VkDescriptorSet globalDescriptorSet(uint32_t frameIndex) const;

    Device    *device_;
    SwapChain *swapChain_;
    FrameSync *frameSync_;
    DescriptorAllocator *descriptorAllocator_;
    MaterialSystem *materialSystem_;

    std::vector<std::unique_ptr<Buffer>> uniformBuffers_;
    std::array<FrameSceneLightStorage, MAX_FRAMES_IN_FLIGHT>
        sceneLightBuffers_{};
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT>
        reflectionProbeBuffers_{};
    uint32_t activeSceneLightCount_ = 0;
    VkDeviceSize                         uniformBufferSize_ = 0;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> globalDescriptorSets_;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Buffer>> atmosphereUniformBuffers_;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>
        atmosphereDescriptorSets_{};
    std::vector<std::unique_ptr<Buffer>> screenSpaceUniformBuffers_;
    VkDescriptorSetLayout screenSpaceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>
        screenSpaceDescriptorSets_{};
    std::shared_ptr<Texture> screenSpaceWhiteFallback_;
    std::shared_ptr<EnvironmentGpuResources> fallbackEnvironment_;
    std::unique_ptr<LightingDescriptorGeneration>
        currentLightingGeneration_;
    std::deque<LightingDescriptorGeneration> retiredLightingGenerations_;
    ReflectionProbeRuntimeStatus reflectionProbeStatus_{};
    std::unique_ptr<RayTracingScene> rayTracingScene_;
    uint64_t nextLightingDescriptorGeneration_ = 1;
    std::unique_ptr<RenderResourceRegistry> renderResources_;
    RendererResourceHandles resourceHandles_{};
    RenderImageHandle activeDirectionalShadowImage_{};
    RenderImageHandle activePointShadowImage_{};
    RenderImageHandle activeSpotShadowImage_{};
    RendererProgramCatalog programs_;
    RenderGraph renderGraph_;
    std::unique_ptr<GpuPassProfiler> gpuPassProfiler_;
    MainForwardPass *mainForwardPass_ = nullptr;
    SurfacePrepass *surfacePrepass_ = nullptr;
    bool lastSurfaceDataActive_ = false;
    ScreenSpaceEffectsStatus screenSpaceStatus_{};
    RenderFeatureRuntimeState featureRuntimeState_{};
    CacaoPass *cacaoPass_ = nullptr;
    GtaoPass *gtaoPass_ = nullptr;
    SsrPass *ssrPass_ = nullptr;
    SsgiPass *ssgiPass_ = nullptr;
    TaaPass *taaPass_ = nullptr;
    DdgiPass *ddgiPass_ = nullptr;
    class OcclusionCullPass *occlusionCullPass_ = nullptr;
    uint32_t lastOcclusionFrameIndex_ = 0;
    uint32_t lastOcclusionRequested_ = 0;
    AtmosphereLutPass *atmosphereLutPass_ = nullptr;
    AtmosphereRuntimeStatus atmosphereStatus_{};
    ToneMapPass *toneMapPass_ = nullptr;
    PresentPass *presentPass_ = nullptr;
};

} // namespace vkr
