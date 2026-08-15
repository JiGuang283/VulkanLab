#pragma once

#include "core/FrameSync.h"
#include "render/FrameGpuData.h"
#include "render/pass/IRenderPass.h"
#include "scene_data/SceneIds.h"

#include <array>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;
class RayTracingScene;

struct DdgiRuntimeStatus {
    bool supported = false;
    bool componentPresent = false;
    bool active = false;
    std::string unavailableReason;
    PersistentEntityId componentEntity{};
    uint32_t probeCount = 0;
    uint32_t raysPerProbe = 0;
    uint32_t probesUpdatedPerFrame = 0;
    uint32_t updateCursor = 0;
    uint32_t tracedInstanceCount = 0;
    uint64_t generation = 0;
    uint64_t resetCount = 0;
    uint64_t allocatedBytes = 0;
};

class DdgiPass final : public IRenderPass {
  public:
    DdgiPass(Device &device, const RenderResourceRegistry &resources,
             RendererResourceHandles handles,
             DescriptorAllocator &descriptorAllocator,
             VkDescriptorSetLayout globalDescriptorSetLayout,
             RayTracingScene &rayTracingScene,
             std::string traceShaderPath,
             std::string updateShaderPath);
    ~DdgiPass() override;

    std::string_view name() const override { return "DDGI"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Ddgi; }
    void prepareGraph(const RenderFrameContext &frame,
                      const RenderResourceRegistry &resources,
                      const VisibilityFrame &visibility) override;
    void prepareFrame(const RenderFrameContext &frame,
                      const RenderResourceRegistry &resources,
                      const VisibilityFrame &visibility) override;
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    uint64_t topologySignature() const override;

    VkDescriptorSetLayout samplingDescriptorSetLayout() const {
        return samplingDescriptorSetLayout_;
    }
    VkDescriptorSet samplingDescriptorSet(uint32_t frameIndex) const {
        return samplingSets_.at(frameIndex);
    }
    const DdgiRuntimeStatus &status() const { return status_; }
    void disableSampling(uint32_t frameIndex,
                         const RenderResourceRegistry &resources);

  private:
    struct FrameStorage {
        std::unique_ptr<Buffer> parameters;
        std::unique_ptr<Buffer> rayResults;
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        uint32_t rayCapacity = 0;
    };

    void createDescriptorLayouts();
    void createPersistentResources(const RenderResourceRegistry &resources);
    void createSamplingDescriptors(const RenderResourceRegistry &resources);
    void updateSamplingDescriptor(uint32_t frameIndex,
                                  const RenderResourceRegistry &resources,
                                  bool active);
    void ensureRayCapacity(uint32_t frameIndex, uint32_t required);
    void updateComputeDescriptor(uint32_t frameIndex,
                                 const RenderResourceRegistry &resources);
    void resetVolume(VkCommandBuffer cmd,
                     const RenderResourceRegistry &resources);
    void recordTrace(const RenderFrameContext &frame);
    void recordUpdate(const RenderFrameContext &frame);
    void finishFrame(const RenderFrameContext &frame);
    void freeDescriptors();

    Device *device_ = nullptr;
    RendererResourceHandles handles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    RayTracingScene *rayTracingScene_ = nullptr;
    std::string traceShaderPath_;
    std::string updateShaderPath_;
    VkDescriptorSetLayout computeDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout samplingDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<FrameStorage, MAX_FRAMES_IN_FLIGHT> frames_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> samplingSets_{};
    std::unique_ptr<Buffer> probeStates_;
    PersistentEntityId volumeEntity_{};
    uint64_t volumeSignature_ = 0;
    uint32_t updateCursor_ = 0;
    bool resetPending_ = true;
    uint32_t preparedFrameIndex_ = 0;
    uint32_t preparedUpdateCount_ = 0;
    uint32_t preparedRayCount_ = 0;
    bool preparedActive_ = false;
    bool preparedReset_ = false;
    DdgiRuntimeStatus status_{};
};

} // namespace vkr
