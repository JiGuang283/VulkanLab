#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"
#include "render/Atmosphere.h"

#include <array>
#include <chrono>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class Image;
class RenderResourceRegistry;

class AtmosphereLutPass final : public IRenderPass {
  public:
    AtmosphereLutPass(
        Device &device, const RenderResourceRegistry &resources,
        RendererResourceHandles resourceHandles,
        DescriptorAllocator &descriptorAllocator,
        VkDescriptorSetLayout atmosphereDescriptorSetLayout,
        std::string transmittanceShaderPath,
        std::string multipleScatteringShaderPath,
        std::string skyViewShaderPath,
        std::string aerialPerspectiveShaderPath);
    ~AtmosphereLutPass() override;

    std::string_view name() const override { return "Atmosphere LUTs"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

    bool readyFor(uint64_t staticLutKey) const;
    const AtmosphereRuntimeStatus &status() const { return status_; }

  private:
    void createStorageDescriptorLayout();
    void createStorageDescriptors(const RenderResourceRegistry &resources);
    void freeStorageDescriptors();
    void transitionImage(VkCommandBuffer cmd, const Image &image,
                         uint32_t arrayLayers, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkAccessFlags sourceAccess,
                         VkAccessFlags destinationAccess,
                         VkPipelineStageFlags sourceStage,
                         VkPipelineStageFlags destinationStage) const;
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
    AtmosphereRuntimeStatus status_{};
};

} // namespace vkr
