#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;
struct FrameRenderFeatures;

class ToneMapPass final : public IRenderPass {
  public:
    ToneMapPass(Device &device, const RenderResourceRegistry &resources,
                RenderImageHandle hdrColor,
                RenderSamplerHandle hdrSampler,
                RenderImageHandle bloomColor,
                RenderSamplerHandle bloomSampler,
                RenderImageHandle viewportColor,
                RenderImageHandle surfaceNormalRoughness,
                RenderImageHandle surfaceMotion,
                RenderSamplerHandle surfaceSampler,
                RenderImageHandle screenDepthPyramid,
                RenderImageHandle sceneColorPyramid,
                RenderImageHandle ssaoRaw,
                RenderImageHandle ssaoFiltered,
                RenderImageHandle cacaoOutput,
                RenderImageHandle taaHistory,
                RenderImageHandle taaDebug,
                RenderSamplerHandle screenPyramidSampler,
                RenderSamplerHandle ssaoSampler,
                RenderSamplerHandle taaSampler,
                DescriptorAllocator &descriptorAllocator,
                std::string fullscreenVertPath,
                std::string toneMapFragPath);
    ~ToneMapPass() override;

    ToneMapPass(const ToneMapPass &) = delete;
    ToneMapPass &operator=(const ToneMapPass &) = delete;

    std::string_view name() const override { return "ToneMap"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();
    void createDescriptors(const RenderResourceRegistry &resources);
    void updateDescriptors(const RenderResourceRegistry &resources);
    void updateScreenDescriptors(const RenderResourceRegistry &resources,
                                 uint32_t frameIndex,
                                 const FrameRenderFeatures &features);

    Device *device_ = nullptr;
    RenderImageHandle hdrColor_{};
    RenderSamplerHandle hdrSampler_{};
    RenderImageHandle bloomColor_{};
    RenderSamplerHandle bloomSampler_{};
    RenderImageHandle viewportColor_{};
    RenderImageHandle surfaceNormalRoughness_{};
    RenderImageHandle surfaceMotion_{};
    RenderSamplerHandle surfaceSampler_{};
    RenderImageHandle screenDepthPyramid_{};
    RenderImageHandle sceneColorPyramid_{};
    RenderImageHandle ssaoRaw_{};
    RenderImageHandle ssaoFiltered_{};
    RenderImageHandle cacaoOutput_{};
    RenderImageHandle taaHistory_{};
    RenderImageHandle taaDebug_{};
    RenderSamplerHandle screenPyramidSampler_{};
    RenderSamplerHandle ssaoSampler_{};
    RenderSamplerHandle taaSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string fullscreenVertPath_;
    std::string toneMapFragPath_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkDescriptorSetLayout sourceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sourceDescriptorSets_{};
};

} // namespace vkr
