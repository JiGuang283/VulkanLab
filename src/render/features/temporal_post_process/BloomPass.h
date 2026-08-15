#pragma once

#include "render/graph/IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourcePool;
struct RenderFrameContext;

class BloomPass final : public IRenderPass {
  public:
    BloomPass(Device &device, const RenderResourcePool &resources,
              RendererResourceHandles resourceHandles,
              DescriptorAllocator &descriptorAllocator,
              std::string downsampleShaderPath,
              std::string upsampleShaderPath);
    ~BloomPass() override;

    BloomPass(const BloomPass &) = delete;
    BloomPass &operator=(const BloomPass &) = delete;

    std::string_view name() const override { return "Bloom"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Bloom; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourcePool &resources) override;
    void onResourceResidencyChanged(
        const RenderResourcePool &resources, uint32_t frameIndex,
        const std::vector<RenderImageHandle> &createdImages) override;

  private:
    static constexpr uint32_t kLevelCount =
        RendererResourceHandles::kBloomPyramidLevelCount;

    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void updatePrimarySource(const RenderResourcePool &resources,
                             uint32_t frameIndex,
                             RenderImageHandle source,
                             RenderSamplerHandle sampler);
    void freeDescriptors();
    uint32_t activeLevelCount(
        const RenderResourcePool &resources) const;
    void recordDownsample(const RenderFrameContext &frame,
                          const RenderResourcePool &resources,
                          uint32_t level);
    void recordUpsample(const RenderFrameContext &frame,
                        const RenderResourcePool &resources,
                        uint32_t destinationLevel);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string downsampleShaderPath_;
    std::string upsampleShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, kLevelCount>,
               MAX_FRAMES_IN_FLIGHT>
        downsampleSets_{};
    std::array<std::array<VkDescriptorSet, kLevelCount - 1>,
               MAX_FRAMES_IN_FLIGHT>
        upsampleSets_{};
};

} // namespace vkr
