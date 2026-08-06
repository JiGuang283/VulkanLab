#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourceRegistry;
struct RenderFrameContext;

class BloomPass final : public IRenderPass {
  public:
    BloomPass(Device &device, const RenderResourceRegistry &resources,
              RendererResourceHandles resourceHandles,
              DescriptorAllocator &descriptorAllocator,
              std::string downsampleShaderPath,
              std::string upsampleShaderPath);
    ~BloomPass() override;

    BloomPass(const BloomPass &) = delete;
    BloomPass &operator=(const BloomPass &) = delete;

    std::string_view name() const override { return "Bloom"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    static constexpr uint32_t kLevelCount =
        RendererResourceHandles::kBloomPyramidLevelCount;

    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void updatePrimarySource(const RenderResourceRegistry &resources,
                             uint32_t frameIndex,
                             RenderImageHandle source,
                             RenderSamplerHandle sampler);
    void freeDescriptors();
    void initializeImages(const RenderFrameContext &frame,
                          const RenderResourceRegistry &resources);
    void prepareImagesForCompute(
        const RenderFrameContext &frame,
        const RenderResourceRegistry &resources) const;
    uint32_t activeLevelCount(
        const RenderResourceRegistry &resources) const;

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
    std::array<bool, MAX_FRAMES_IN_FLIGHT> initialized_{};
};

} // namespace vkr
