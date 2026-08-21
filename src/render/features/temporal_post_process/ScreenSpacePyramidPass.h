#pragma once

#include "render/graph/IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourcePool;

class ScreenSpacePyramidPass final : public IRenderPass {
  public:
    ScreenSpacePyramidPass(Device &device,
                           const RenderResourcePool &resources,
                           RenderImageHandle source,
                           RenderSamplerHandle sourceSampler,
                           RenderImageHandle alternateSource,
                           RenderSamplerHandle alternateSourceSampler,
                           RenderImageHandle pyramid,
                           RenderSamplerHandle pyramidSampler,
                           DescriptorAllocator &descriptorAllocator,
                           std::string initShaderPath,
                           std::string reduceShaderPath);
    ~ScreenSpacePyramidPass() override;

    std::string_view name() const override { return name_; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return RgPassCondition::SceneColorPyramid;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourcePool &resources) override;
    void onResourceResidencyChanged(
        const RenderResourcePool &resources, uint32_t frameIndex,
        const std::vector<RenderImageHandle> &createdImages) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void updateInitialSource(const RenderResourcePool &resources,
                             uint32_t frameIndex,
                             bool useAlternateSource);
    void freeDescriptors();
    void recordMip(const RenderFrameContext &frame,
                   const RenderResourcePool &resources,
                   uint32_t mip);

    Device *device_ = nullptr;
    std::string name_;
    RenderImageHandle source_{};
    RenderSamplerHandle sourceSampler_{};
    RenderImageHandle alternateSource_{};
    RenderSamplerHandle alternateSourceSampler_{};
    RenderImageHandle pyramid_{};
    RenderSamplerHandle pyramidSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string initShaderPath_;
    std::string reduceShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::vector<VkDescriptorSet>, MAX_FRAMES_IN_FLIGHT> sets_{};
};

} // namespace vkr
