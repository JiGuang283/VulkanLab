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
class RenderResourceRegistry;

class HiZBuildPass final : public IRenderPass {
  public:
    HiZBuildPass(Device &device,
                 const RenderResourceRegistry &resources,
                 RendererResourceHandles resourceHandles,
                 DescriptorAllocator &descriptorAllocator,
                 std::string initShaderPath,
                 std::string reduceShaderPath);
    ~HiZBuildPass() override;

    std::string_view name() const override { return "HiZBuild"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::HiZ; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();
    void recordMip(const RenderFrameContext &frame,
                   const RenderResourceRegistry &resources,
                   const VisibilityFrame &visibility, uint32_t mip);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string initShaderPath_;
    std::string reduceShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::vector<VkDescriptorSet>, MAX_FRAMES_IN_FLIGHT> sets_{};
};

} // namespace vkr
