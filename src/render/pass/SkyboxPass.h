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

class SkyboxPass final : public IRenderPass {
  public:
    SkyboxPass(Device &device,
               const RenderResourceRegistry &resources,
               RendererResourceHandles resourceHandles,
               VkDescriptorSetLayout globalDescriptorSetLayout,
               VkDescriptorSetLayout lightingDescriptorSetLayout,
               std::string vertexShaderPath,
               std::string fragmentShaderPath);
    ~SkyboxPass() override;

    std::string_view name() const override { return "Skybox"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout emptyDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string vertexShaderPath_;
    std::string fragmentShaderPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
