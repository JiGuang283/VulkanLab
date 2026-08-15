#pragma once

#include "render/pass/IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourceRegistry;

class SsaoPass final : public IRenderPass {
  public:
    SsaoPass(Device &device, const RenderResourceRegistry &resources,
             RendererResourceHandles resourceHandles,
             DescriptorAllocator &descriptorAllocator,
             VkDescriptorSetLayout globalDescriptorSetLayout,
             std::string traceShaderPath, std::string blurShaderPath);
    ~SsaoPass() override;

    std::string_view name() const override { return "SSAO"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Ssao; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();
    void recordStage(const RenderFrameContext &frame,
                     const RenderResourceRegistry &resources,
                     uint32_t stage);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::string traceShaderPath_;
    std::string blurShaderPath_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> traceSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> horizontalSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> verticalSets_{};
};

} // namespace vkr
