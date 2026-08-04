#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"
#include "render/Visibility.h"

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
struct RenderView;

class OcclusionCullPass final : public IRenderPass {
  public:
    static constexpr uint32_t kMaxCandidates = 65536;

    OcclusionCullPass(Device &device,
                      const RenderResourceRegistry &resources,
                      RendererResourceHandles resourceHandles,
                      DescriptorAllocator &descriptorAllocator,
                      std::string computeShaderPath);
    ~OcclusionCullPass() override;

    std::string_view name() const override { return "OcclusionCull"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

    void prepareFrame(uint32_t frameIndex, uint64_t frameSerial,
                      const VisibilityFrame &visibility,
                      const RenderView &view);
    bool active(uint32_t frameIndex) const;
    const GpuVisibilityDrawStream &drawStream(uint32_t frameIndex) const;
    const CompletedGpuVisibilityStatistics &completedStatistics() const {
        return completedStatistics_;
    }
    uint32_t capacity(uint32_t frameIndex) const;
    uint64_t allocatedBytes() const;

  private:
    struct FrameStorage;
    void createDescriptorSetLayout();
    void createDescriptorSets(const RenderResourceRegistry &resources);
    void freeDescriptorSets();
    void updateDescriptor(uint32_t frameIndex,
                          const RenderResourceRegistry &resources);
    void ensureCapacity(uint32_t frameIndex, uint32_t required);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string computeShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    std::array<std::unique_ptr<FrameStorage>, MAX_FRAMES_IN_FLIGHT> frames_{};
    const RenderResourceRegistry *resources_ = nullptr;
    CompletedGpuVisibilityStatistics completedStatistics_{};
};

} // namespace vkr
