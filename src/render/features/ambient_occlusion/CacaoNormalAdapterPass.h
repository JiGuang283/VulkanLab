#pragma once

#include "core/FrameSync.h"
#include "render/graph/IRenderPass.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourcePool;

class CacaoNormalAdapterPass final : public IRenderPass {
  public:
    CacaoNormalAdapterPass(Device &device,
                           const RenderResourcePool &resources,
                           RendererResourceHandles resourceHandles,
                           DescriptorAllocator &descriptorAllocator,
                           VkDescriptorSetLayout globalDescriptorSetLayout,
                           std::string shaderPath);
    ~CacaoNormalAdapterPass() override;

    std::string_view name() const override { return "CACAO Input Adapter"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Cacao; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourcePool &resources) override;
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void freeDescriptors();

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::string shaderPath_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
};

} // namespace vkr
