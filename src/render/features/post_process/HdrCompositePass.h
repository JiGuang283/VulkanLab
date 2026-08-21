#pragma once

#include "render/graph/IRenderPass.h"

#include <array>

namespace vkr {

class Device;
class DescriptorAllocator;
struct OpaqueRenderProducts;

class HdrCompositePass final : public IRenderPass {
  public:
    HdrCompositePass(Device &device, const RenderResourcePool &registry,
                     RendererResourceHandles resources,
                     DescriptorAllocator &descriptorAllocator,
                     std::string shaderPath);
    ~HdrCompositePass() override;

    std::string_view name() const override {
        return "ScreenSpaceLightingComposite";
    }
    RgPassType passType() const override { return RgPassType::Compute; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourcePool &resources) override;
    void onResourceResidencyChanged(
        const RenderResourcePool &resources, uint32_t frameIndex,
        const std::vector<RenderImageHandle> &createdImages) override;

  private:
    Device *device_ = nullptr;
    RendererResourceHandles resources_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string shaderPath_;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};

    void createLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void updateDescriptor(const RenderResourcePool &resources,
                          uint32_t frameIndex, bool ssrActive,
                          bool ssgiActive,
                          const OpaqueRenderProducts &products);
    void recordComposite(const RenderFrameContext &frame,
                         const RenderResourcePool &resources);
    void freeDescriptors();
};

} // namespace vkr
