#pragma once

#include "render/graph/IRenderPass.h"
#include "core/FrameSync.h"
#include "render/features/atmosphere_environment/Atmosphere.h"

#include <array>
#include <chrono>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class Image;
class RenderResourcePool;

class AtmosphereLutPass final : public IRenderPass {
  public:
    AtmosphereLutPass(
        Device &device, const RenderResourcePool &resources,
        RendererResourceHandles resourceHandles,
        DescriptorAllocator &descriptorAllocator,
        VkDescriptorSetLayout atmosphereDescriptorSetLayout,
        std::string transmittanceShaderPath,
        std::string multipleScatteringShaderPath,
        std::string skyViewShaderPath,
        std::string aerialPerspectiveShaderPath);
    ~AtmosphereLutPass() override;

    std::string_view name() const override { return "Atmosphere LUTs"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return RgPassCondition::Atmosphere;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void onResourceResidencyChanged(
        const RenderResourcePool &resources, uint32_t frameIndex,
        const std::vector<RenderImageHandle> &createdImages) override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

    bool readyFor(uint64_t staticLutKey) const;
    const AtmosphereRuntimeStatus &status() const { return status_; }

  private:
    void createStorageDescriptorLayout();
    void createStorageDescriptors(const RenderResourcePool &resources);
    void freeStorageDescriptors();
    void updateFrameState(const RenderFrameContext &frame);
    void recordStage(const RenderFrameContext &frame,
                     const RenderResourcePool &resources,
                     uint32_t stage);
    void dispatch(const RenderFrameContext &frame, std::string_view debugName,
                  const std::string &shaderPath, VkDescriptorSet storageSet,
                  VkExtent2D extent, uint32_t layers) const;

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout storageDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet transmittanceStorageSet_ = VK_NULL_HANDLE;
    VkDescriptorSet multipleScatteringStorageSet_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> skyViewStorageSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> aerialStorageSets_{};
    std::string transmittanceShaderPath_;
    std::string multipleScatteringShaderPath_;
    std::string skyViewShaderPath_;
    std::string aerialPerspectiveShaderPath_;
    uint64_t currentStaticLutKey_ = 0;
    uint64_t pendingStaticLutKey_ = 0;
    std::chrono::steady_clock::time_point pendingSince_{};
    bool buildStaticThisFrame_ = false;
    std::chrono::steady_clock::time_point staticBuildStarted_{};
    AtmosphereRuntimeStatus status_{};
};

} // namespace vkr
