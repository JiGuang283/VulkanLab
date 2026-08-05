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
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();

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
    std::array<bool, MAX_FRAMES_IN_FLIGHT> initialized_{};
};

} // namespace vkr
