#pragma once

#include "render/graph/IRenderPass.h"
#include "render/features/surface/GBufferResources.h"
#include "render/features/deferred/DeferredLightingResources.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourcePool;
class SwapChain;
struct RenderFrameContext;
struct FrameRenderFeatures;
enum class ScreenSpaceDebugView;
enum class GBufferDebugView;
enum class DeferredLightingDebugView;

class ToneMapPass final : public IRenderPass {
  public:
    ToneMapPass(Device &device, const RenderResourcePool &resources,
                RenderImageHandle hdrColor,
                RenderImageHandle compositedHdrColor,
                RenderSamplerHandle hdrSampler,
                RenderImageHandle bloomColor,
                RenderSamplerHandle bloomSampler,
                RenderImageHandle viewportColor,
                RenderImageHandle surfaceNormalRoughness,
                RenderImageHandle surfaceMotion,
                RenderSamplerHandle surfaceSampler,
                GBufferResources gBuffer,
                DeferredLightingResources deferredLighting,
                RenderImageHandle screenDepthPyramid,
                RenderImageHandle sceneColorPyramid,
                RenderImageHandle ssaoRaw,
                RenderImageHandle ssaoFiltered,
                RenderImageHandle cacaoOutput,
                RenderImageHandle gtaoRaw,
                RenderImageHandle gtaoHistory,
                RenderImageHandle gtaoFiltered,
                RenderImageHandle gtaoDebug,
                RenderImageHandle taaHistory,
                RenderImageHandle taaDebug,
                RenderImageHandle ssrRaw,
                RenderImageHandle ssrHistory,
                RenderImageHandle ssrFiltered,
                RenderImageHandle ssrDebug,
                RenderImageHandle ssgiRaw,
                RenderImageHandle ssgiHistory,
                RenderImageHandle ssgiFiltered,
                RenderImageHandle ssgiDebug,
                RenderSamplerHandle screenPyramidSampler,
                RenderSamplerHandle ssaoSampler,
                RenderSamplerHandle taaSampler,
                RenderSamplerHandle ssrSampler,
                RenderSamplerHandle ssgiSampler,
                DescriptorAllocator &descriptorAllocator,
                std::string fullscreenVertPath,
                std::string toneMapFragPath);
    ~ToneMapPass() override;

    ToneMapPass(const ToneMapPass &) = delete;
    ToneMapPass &operator=(const ToneMapPass &) = delete;

    std::string_view name() const override { return "ToneMap"; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void onViewportResize(
        const RenderResourcePool &resources) override;
    void onResourceResidencyChanged(
        const RenderResourcePool &, uint32_t,
        const std::vector<RenderImageHandle> &) override {}
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    void createDescriptors(const RenderResourcePool &resources);
    void updateDescriptors(const RenderResourcePool &resources);
    void updateScreenDescriptors(const RenderResourcePool &resources,
                                 uint32_t frameIndex,
                                 const FrameRenderFeatures &features,
                                 ScreenSpaceDebugView debugView,
                                 GBufferDebugView gBufferDebugView,
                                 DeferredLightingDebugView deferredDebugView);

    Device *device_ = nullptr;
    RenderImageHandle hdrColor_{};
    RenderImageHandle compositedHdrColor_{};
    RenderSamplerHandle hdrSampler_{};
    RenderImageHandle bloomColor_{};
    RenderSamplerHandle bloomSampler_{};
    RenderImageHandle viewportColor_{};
    RenderImageHandle surfaceNormalRoughness_{};
    RenderImageHandle surfaceMotion_{};
    RenderSamplerHandle surfaceSampler_{};
    GBufferResources gBuffer_{};
    DeferredLightingResources deferredLighting_{};
    RenderImageHandle screenDepthPyramid_{};
    RenderImageHandle sceneColorPyramid_{};
    RenderImageHandle ssaoRaw_{};
    RenderImageHandle ssaoFiltered_{};
    RenderImageHandle cacaoOutput_{};
    RenderImageHandle gtaoRaw_{};
    RenderImageHandle gtaoHistory_{};
    RenderImageHandle gtaoFiltered_{};
    RenderImageHandle gtaoDebug_{};
    RenderImageHandle taaHistory_{};
    RenderImageHandle taaDebug_{};
    RenderImageHandle ssrRaw_{};
    RenderImageHandle ssrHistory_{};
    RenderImageHandle ssrFiltered_{};
    RenderImageHandle ssrDebug_{};
    RenderImageHandle ssgiRaw_{};
    RenderImageHandle ssgiHistory_{};
    RenderImageHandle ssgiFiltered_{};
    RenderImageHandle ssgiDebug_{};
    RenderSamplerHandle screenPyramidSampler_{};
    RenderSamplerHandle ssaoSampler_{};
    RenderSamplerHandle taaSampler_{};
    RenderSamplerHandle ssrSampler_{};
    RenderSamplerHandle ssgiSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string fullscreenVertPath_;
    std::string toneMapFragPath_;

    VkDescriptorSetLayout sourceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sourceDescriptorSets_{};
};

} // namespace vkr
