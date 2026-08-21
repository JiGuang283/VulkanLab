#pragma once

#include "core/FrameSync.h"
#include "render/graph/IRenderPass.h"

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderResourcePool;
class SurfaceFrameData;

class SurfacePrepass final : public IRenderPass {
  public:
    SurfacePrepass(Device &device,
                   const RenderResourcePool &resources,
                   RendererResourceHandles resourceHandles,
                   SurfaceFrameData &frameData,
                   VkDescriptorSetLayout globalDescriptorSetLayout);
    ~SurfacePrepass() override = default;

    std::string_view name() const override { return "SurfacePrepass"; }
    RgPassCondition condition() const override {
        return RgPassCondition::SurfaceData;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

    uint32_t historyCapacity(uint32_t frameIndex) const;
    uint64_t allocatedBytes() const;

  private:
    void draw(const RenderFrameContext &frame,
              const RenderResourcePool &resources,
              const VisibilityFrame &visibility);

    Device *device_ = nullptr;
    SurfaceFrameData *frameData_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
