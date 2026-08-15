#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;
class RenderResourceRegistry;

class SurfacePrepass final : public IRenderPass {
  public:
    SurfacePrepass(Device &device,
                   const RenderResourceRegistry &resources,
                   RendererResourceHandles resourceHandles,
                   DescriptorAllocator &descriptorAllocator,
                   VkDescriptorSetLayout globalDescriptorSetLayout,
                   std::string vertexShaderPath,
                   std::string opaqueFragmentShaderPath,
                   std::string maskFragmentShaderPath);
    ~SurfacePrepass() override;

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
    struct FrameStorage;

    void createDescriptorSetLayout();
    void createFrameStorage();
    void updateDescriptor(uint32_t frameIndex);
    void ensureHistoryCapacity(uint32_t frameIndex, uint32_t required);
    void prepareFrame(uint32_t frameIndex,
                      const VisibilityFrame &visibility,
                      VkExtent2D extent);
    void draw(const RenderFrameContext &frame,
              const RenderResourceRegistry &resources,
              const VisibilityFrame &visibility);

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout surfaceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string vertexShaderPath_;
    std::string opaqueFragmentShaderPath_;
    std::string maskFragmentShaderPath_;
    std::array<std::unique_ptr<FrameStorage>, MAX_FRAMES_IN_FLIGHT> frames_{};
};

} // namespace vkr
