#pragma once

#include "render/pass/IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourceRegistry;

enum class ScreenSpacePyramidKind {
    NearestDepth,
    SceneColor,
};

class ScreenSpacePyramidPass final : public IRenderPass {
  public:
    ScreenSpacePyramidPass(Device &device,
                           const RenderResourceRegistry &resources,
                           ScreenSpacePyramidKind kind,
                           RenderImageHandle source,
                           RenderSamplerHandle sourceSampler,
                           RenderImageHandle alternateSource,
                           RenderSamplerHandle alternateSourceSampler,
                           RenderImageHandle pyramid,
                           RenderSamplerHandle pyramidSampler,
                           DescriptorAllocator &descriptorAllocator,
                           std::string initShaderPath,
                           std::string reduceShaderPath);
    ~ScreenSpacePyramidPass() override;

    std::string_view name() const override { return name_; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return kind_ == ScreenSpacePyramidKind::NearestDepth
                   ? RgPassCondition::ScreenDepthPyramid
                   : RgPassCondition::SceneColorPyramid;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;

  private:
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourceRegistry &resources);
    void updateInitialSource(const RenderResourceRegistry &resources,
                             uint32_t frameIndex,
                             bool useAlternateSource);
    void freeDescriptors();
    void recordMip(const RenderFrameContext &frame,
                   const RenderResourceRegistry &resources,
                   uint32_t mip);

    Device *device_ = nullptr;
    ScreenSpacePyramidKind kind_ = ScreenSpacePyramidKind::NearestDepth;
    std::string name_;
    RenderImageHandle source_{};
    RenderSamplerHandle sourceSampler_{};
    RenderImageHandle alternateSource_{};
    RenderSamplerHandle alternateSourceSampler_{};
    RenderImageHandle pyramid_{};
    RenderSamplerHandle pyramidSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string initShaderPath_;
    std::string reduceShaderPath_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::vector<VkDescriptorSet>, MAX_FRAMES_IN_FLIGHT> sets_{};
};

} // namespace vkr
