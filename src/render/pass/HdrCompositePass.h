#pragma once

#include "render/pass/IRenderPass.h"

#include <array>

namespace vkr {

class Device;
class DescriptorAllocator;

class HdrCompositePass final : public IRenderPass {
  public:
    HdrCompositePass(Device &device, const RenderResourceRegistry &registry,
                     RendererResourceHandles resources,
                     DescriptorAllocator &descriptorAllocator,
                     std::string shaderPath);
    ~HdrCompositePass() override;

    std::string_view name() const override { return "ReflectionComposite"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    Device *device_ = nullptr;
    RendererResourceHandles resources_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string shaderPath_;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    std::array<bool, MAX_FRAMES_IN_FLIGHT> initialized_{};

    void createLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();
};

} // namespace vkr
