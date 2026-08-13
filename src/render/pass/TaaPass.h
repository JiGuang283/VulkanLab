#pragma once

#include "render/RenderResourceRegistry.h"
#include "render/pass/IRenderPass.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;

struct TaaPassStatus {
    bool supported = false;
    bool active = false;
    bool historyValid = false;
    uint64_t historyGeneration = 0;
    uint64_t lastFrameSerial = 0;
    glm::vec2 jitterPixels{0.0f};
    std::string lastResetReason = "initial frame";
};

class TaaPass final : public IRenderPass {
  public:
    TaaPass(Device &device, const RenderResourceRegistry &resources,
            RendererResourceHandles resourceHandles,
            DescriptorAllocator &descriptorAllocator,
            std::string resolveShaderPath);
    ~TaaPass() override;

    std::string_view name() const override { return "TAA"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override { return RgPassCondition::Taa; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

    const TaaPassStatus &status() const { return status_; }

  private:
    void createDescriptorSetLayout();
    void createFrameBuffers();
    void createDescriptors(const RenderResourceRegistry &resources);
    void freeDescriptors();

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string resolveShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> frameUbos_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    std::array<bool, MAX_FRAMES_IN_FLIGHT> historyWritten_{};
    uint64_t lastExecutionSerial_ = 0;
    uint64_t lastHistoryGeneration_ = 0;
    TaaPassStatus status_{};
};

} // namespace vkr
