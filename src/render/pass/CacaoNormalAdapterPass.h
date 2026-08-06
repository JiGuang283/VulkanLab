#pragma once

#include "core/FrameSync.h"
#include "render/pass/IRenderPass.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourceRegistry;

class CacaoNormalAdapterPass final : public IRenderPass {
  public:
    CacaoNormalAdapterPass(Device &device,
                           const RenderResourceRegistry &resources,
                           RendererResourceHandles resourceHandles,
                           DescriptorAllocator &descriptorAllocator,
                           VkDescriptorSetLayout globalDescriptorSetLayout,
                           std::string shaderPath);
    ~CacaoNormalAdapterPass() override;

    std::string_view name() const override { return "CACAO Input Adapter"; }
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
    std::string shaderPath_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    std::array<bool, MAX_FRAMES_IN_FLIGHT> initialized_{};
};

} // namespace vkr
