#pragma once

#include "core/FrameSync.h"
#include "render/graph/RenderResourcePool.h"
#include "render/graph/IRenderPass.h"

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
    SsrPass(Device &device, const RenderResourcePool &resources,
            RendererResourceHandles resourceHandles,
            DescriptorAllocator &descriptorAllocator,
            VkDescriptorSetLayout globalDescriptorSetLayout,
            std::string traceShaderPath, std::string temporalShaderPath,
            std::string blurShaderPath);
    ~SsrPass() override;

    std::string_view name() const override { return "SSR"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Ssr; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourcePool &resources) override;

    const SsrPassStatus &status() const { return status_; }

  private:
    void createLayouts();
    void createDescriptors(const RenderResourcePool &resources);
    void updateTemporalHistoryDescriptors(
        const RenderResourcePool &resources, uint32_t current,
        uint32_t previous, bool historyValid);
    void beginFrame(const RenderFrameContext &frame,
                    const RenderResourcePool &resources,
                    const VisibilityFrame &visibility);
    void recordStage(const RenderFrameContext &frame,
                     const RenderResourcePool &resources,
                     uint32_t stage);
    void finishFrame(const RenderFrameContext &frame,
                     const VisibilityFrame &visibility);
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
    std::array<bool, MAX_FRAMES_IN_FLIGHT> historyWritten_{};
    bool currentHistoryValid_ = false;
    uint64_t currentSettingsSignature_ = 0;
    uint64_t lastExecutionSerial_ = 0;
    uint64_t lastHistoryGeneration_ = 0;
    uint64_t lastSettingsSignature_ = 0;
    SsrPassStatus status_{};
};

} // namespace vkr
