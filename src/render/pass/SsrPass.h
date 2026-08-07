#pragma once

#include "core/FrameSync.h"
#include "render/RenderResourceRegistry.h"
#include "render/pass/IRenderPass.h"

#include <array>
#include <string>

namespace vkr {

class DescriptorAllocator;
class Device;

struct SsrPassStatus {
    bool supported = false;
    bool active = false;
    bool historyValid = false;
    uint64_t historyGeneration = 0;
    uint64_t lastFrameSerial = 0;
    VkExtent2D extent{};
    std::string lastResetReason = "initial frame";
};

class SsrPass final : public IRenderPass {
  public:
    SsrPass(Device &device, const RenderResourceRegistry &resources,
            RendererResourceHandles resourceHandles,
            DescriptorAllocator &descriptorAllocator,
            VkDescriptorSetLayout globalDescriptorSetLayout,
            std::string traceShaderPath, std::string temporalShaderPath,
            std::string blurShaderPath);
    ~SsrPass() override;

    std::string_view name() const override { return "SSR"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

    const SsrPassStatus &status() const { return status_; }

  private:
    void createLayouts();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();

    Device *device_ = nullptr;
    RendererResourceHandles resources_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout traceLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout temporalLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blurLayout_ = VK_NULL_HANDLE;
    std::string traceShaderPath_;
    std::string temporalShaderPath_;
    std::string blurShaderPath_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> traceSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> temporalSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> horizontalSets_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> verticalSets_{};
    std::array<bool, MAX_FRAMES_IN_FLIGHT> initialized_{};
    std::array<bool, MAX_FRAMES_IN_FLIGHT> historyWritten_{};
    uint64_t lastExecutionSerial_ = 0;
    uint64_t lastHistoryGeneration_ = 0;
    uint64_t lastSettingsSignature_ = 0;
    SsrPassStatus status_{};
};

} // namespace vkr
