#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "render/RenderPipeline.h"
#include "render/FrameGpuData.h"
#include "render/GpuPassProfiler.h"
#include "render/RenderResourceRegistry.h"
#include "render/RendererShaderPaths.h"
#include "render/Atmosphere.h"
#include "render/Visibility.h"

#include <memory>
#include <array>
#include <deque>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
struct EnvironmentGpuResources;
class GuiSystem;
class MainForwardPass;
class SurfacePrepass;
class AtmosphereLutPass;
class ToneMapPass;
class PresentPass;
class PipelineCache;
struct ShaderVariant;
struct RenderView;

struct RendererViewportOutput {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<VkImage, MAX_FRAMES_IN_FLIGHT> images{};
    std::array<VkImageView, MAX_FRAMES_IN_FLIGHT> imageViews{};
};

struct SceneLightBufferStatus {
    uint32_t limit = kMaxSceneLights;
    uint32_t activeLights = 0;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> frameCapacities{};
    uint64_t allocatedBytes = 0;
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

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             DescriptorAllocator &descriptorAllocator,
             RendererShaderPaths shaderPaths);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void renderFrame(const FrameSync::FrameContext &frame,
                     const VisibilityFrame &visibility,
                     PipelineCache &pipelineCache,
                     GuiSystem *gui, const ShaderVariant &shaderVariant,
                     const RenderView &view);

    // ---- 交换链重建 ----
    void recreateSwapChain();
    void resizeViewport(VkExtent2D extent);

    // ---- 访问器 ----
    VkRenderPass renderPass() const;
    VkExtent2D viewportExtent() const;
    RendererViewportOutput viewportOutput() const;
    const GpuPassTimings &gpuPassTimings() const;
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
    OcclusionCullingStatus occlusionCullingStatus() const;
    SurfaceDataStatus surfaceDataStatus() const;
    bool atmosphereSupported() const;
    const std::string &atmosphereUnsupportedReason() const;
    AtmosphereRuntimeStatus atmosphereStatus() const;

    // ---- per-frame UBO 访问器 ----
  private:
    struct FrameSceneLightStorage {
        std::unique_ptr<Buffer> buffer;
        uint32_t capacity = 0;
    };

    struct LightingDescriptorGeneration {
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
        std::shared_ptr<EnvironmentGpuResources> environment;
        uint64_t retireAfterSerial = 0;
    };

    void createUniformBuffers();
    void createSceneLightBuffers();
    void ensureSceneLightCapacity(uint32_t frameIndex,
                                  uint32_t requiredLights);
    void updateSceneLightDescriptor(uint32_t frameIndex);
    void createGlobalDescriptorSetLayout();
    void createGlobalDescriptorSets();
    void createLightingDescriptorSetLayout();
    void createAtmosphereUniformBuffers();
    void createAtmosphereDescriptorSetLayout();
    void createAtmosphereDescriptorSets();
    void initializeAtmosphereImages();
    void createFallbackEnvironment();
    void createLightingGeneration(
        std::shared_ptr<EnvironmentGpuResources> environment);
    void collectRetiredLightingGenerations();
    void freeLightingGeneration(
        LightingDescriptorGeneration &generation);
    void createRenderPipeline();
    VkDescriptorSet globalDescriptorSet(uint32_t frameIndex) const;

    Device    *device_;
    SwapChain *swapChain_;
    FrameSync *frameSync_;
    DescriptorAllocator *descriptorAllocator_;

    std::vector<std::unique_ptr<Buffer>> uniformBuffers_;
    std::array<FrameSceneLightStorage, MAX_FRAMES_IN_FLIGHT>
        sceneLightBuffers_{};
    uint32_t activeSceneLightCount_ = 0;
    VkDeviceSize                         uniformBufferSize_ = 0;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> globalDescriptorSets_;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Buffer>> atmosphereUniformBuffers_;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>
        atmosphereDescriptorSets_{};
    std::shared_ptr<EnvironmentGpuResources> fallbackEnvironment_;
    std::unique_ptr<LightingDescriptorGeneration>
        currentLightingGeneration_;
    std::deque<LightingDescriptorGeneration> retiredLightingGenerations_;
    std::unique_ptr<RenderResourceRegistry> renderResources_;
    RendererResourceHandles resourceHandles_{};
    RendererShaderPaths shaderPaths_;
    RenderPipeline pipeline_;
    std::unique_ptr<GpuPassProfiler> gpuPassProfiler_;
    MainForwardPass *mainForwardPass_ = nullptr;
    SurfacePrepass *surfacePrepass_ = nullptr;
    bool lastSurfaceDataActive_ = false;
    class OcclusionCullPass *occlusionCullPass_ = nullptr;
    uint32_t lastOcclusionFrameIndex_ = 0;
    uint32_t lastOcclusionRequested_ = 0;
    AtmosphereLutPass *atmosphereLutPass_ = nullptr;
    AtmosphereRuntimeStatus atmosphereStatus_{};
    ToneMapPass *toneMapPass_ = nullptr;
    PresentPass *presentPass_ = nullptr;
};

} // namespace vkr
