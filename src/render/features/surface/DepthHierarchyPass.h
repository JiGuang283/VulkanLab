#pragma once

#include "core/FrameSync.h"
#include "render/features/surface/DepthHierarchyResources.h"
#include "render/graph/IRenderPass.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderResourcePool;
struct FrameRenderFeatures;

struct DepthHierarchyPrograms {
    std::string combinedInit;
    std::string combinedReduce;
    std::string nearestInit;
    std::string nearestReduce;
    std::string farthestInit;
    std::string farthestReduce;
};

class DepthHierarchyPass final : public IRenderPass {
  public:
    DepthHierarchyPass(Device &device,
                       const RenderResourcePool &resources,
                       RenderImageHandle sourceDepth,
                       RenderSamplerHandle sourceDepthSampler,
                       DepthHierarchyResources hierarchy,
                       DescriptorAllocator &descriptorAllocator,
                       DepthHierarchyPrograms programs);
    ~DepthHierarchyPass() override;

    std::string_view name() const override { return "DepthHierarchy"; }
    RgPassType passType() const override { return RgPassType::Compute; }
    RgPassCondition condition() const override {
        return RgPassCondition::DepthHierarchy;
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

    bool combined() const { return hierarchy_.combined(); }

  private:
    enum class Chain : uint32_t { Combined = 0, Nearest = 1, Farthest = 2 };
    static constexpr uint32_t kLocalNodeStride = 64;

    struct ChainState {
        RenderImageHandle image{};
        RenderSamplerHandle sampler{};
        std::string name;
        std::string initShader;
        std::string reduceShader;
        std::array<std::vector<VkDescriptorSet>, MAX_FRAMES_IN_FLIGHT> sets{};
    };

    bool chainRequired(Chain chain,
                       const FrameRenderFeatures &features) const;
    const ChainState &chain(Chain value) const;
    ChainState &chain(Chain value);
    void createDescriptorSetLayout();
    void createDescriptors(const RenderResourcePool &resources);
    void createChainDescriptors(const RenderResourcePool &resources,
                                Chain value);
    void freeChainDescriptors(Chain value);
    void freeDescriptors();
    void recordMip(const RenderFrameContext &frame,
                   const RenderResourcePool &resources,
                   Chain chain, uint32_t mip);

    Device *device_ = nullptr;
    RenderImageHandle sourceDepth_{};
    RenderSamplerHandle sourceDepthSampler_{};
    DepthHierarchyResources hierarchy_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<ChainState, 3> chains_{};
};

} // namespace vkr
