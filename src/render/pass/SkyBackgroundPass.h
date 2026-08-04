#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class SkyBackgroundPass final : public IRenderPass {
  public:
    SkyBackgroundPass(Device &device,
               const RenderResourceRegistry &resources,
               RendererResourceHandles resourceHandles,
               VkDescriptorSetLayout globalDescriptorSetLayout,
               VkDescriptorSetLayout lightingDescriptorSetLayout,
               VkDescriptorSetLayout atmosphereDescriptorSetLayout,
               std::string vertexShaderPath,
               std::string skyboxFragmentShaderPath,
               std::string atmosphereFragmentShaderPath);
    ~SkyBackgroundPass() override;

    std::string_view name() const override { return "SkyBackground"; }
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

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout emptyDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string vertexShaderPath_;
    std::string skyboxFragmentShaderPath_;
    std::string atmosphereFragmentShaderPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
