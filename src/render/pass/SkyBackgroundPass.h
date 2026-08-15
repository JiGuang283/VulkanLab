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
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout emptyDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string vertexShaderPath_;
    std::string skyboxFragmentShaderPath_;
    std::string atmosphereFragmentShaderPath_;
};

} // namespace vkr
