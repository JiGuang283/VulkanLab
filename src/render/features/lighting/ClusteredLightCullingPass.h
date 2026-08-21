#pragma once

#include "render/graph/IRenderPass.h"

#include <string>

namespace vkr {

class ClusteredLightingResources;
class Device;

class ClusteredLightCullingPass final : public IRenderPass {
  public:
    ClusteredLightCullingPass(Device &device,
                              ClusteredLightingResources &resources,
                              VkDescriptorSetLayout globalLayout,
                              std::string shaderPath);

    std::string_view name() const override { return "ClusteredLighting"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return RgPassCondition::ClusteredLighting;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    uint64_t topologySignature() const override;

  private:
    Device *device_ = nullptr;
    ClusteredLightingResources *resources_ = nullptr;
    VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;
    std::string shaderPath_;
};

} // namespace vkr
