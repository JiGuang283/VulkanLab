#pragma once

#include "core/FrameSync.h"
#include "render/graph/IRenderPass.h"

#include <array>
#include <string>

namespace vkr {

class DescriptorAllocator;
class Device;
class ClusteredLightingResources;

struct DeferredLightingStatus {
    bool supported = false;
    bool active = false;
    VkExtent2D extent{};
    uint32_t dispatchX = 0;
    uint32_t dispatchY = 0;
    uint64_t residentBytes = 0;
    std::string unavailableReason;
};

class DeferredLightingPass final : public IRenderPass {
  public:
    DeferredLightingPass(
        Device &device, const RenderResourcePool &resources,
        RendererResourceHandles resourceHandles,
        DescriptorAllocator &descriptorAllocator,
        VkDescriptorSetLayout globalLayout,
        VkDescriptorSetLayout lightingLayout,
        VkDescriptorSetLayout atmosphereLayout,
        VkDescriptorSetLayout screenSpaceLayout,
        VkDescriptorSetLayout ddgiLayout,
        ClusteredLightingResources &clusteredLighting,
        std::string shaderPath);
    ~DeferredLightingPass() override;

    std::string_view name() const override { return "DeferredLighting"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return RgPassCondition::DeferredLighting;
    }
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

    const DeferredLightingStatus &status() const { return status_; }

  private:
    bool resourcesReady(const RenderResourcePool &resources) const;
    void createLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void ensureDescriptor(const RenderResourcePool &resources,
                          uint32_t frameIndex);
    void updateDescriptor(const RenderResourcePool &resources,
                          uint32_t frameIndex);
    void freeDescriptors();

    Device *device_ = nullptr;
    RendererResourceHandles resources_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout screenSpaceLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ddgiLayout_ = VK_NULL_HANDLE;
    ClusteredLightingResources *clusteredLighting_ = nullptr;
    VkDescriptorSetLayout localLayout_ = VK_NULL_HANDLE;
    std::string shaderPath_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    DeferredLightingStatus status_{};
};

} // namespace vkr
