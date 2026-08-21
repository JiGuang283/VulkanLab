#include "render/features/surface/DepthHierarchyPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "render/pipeline/PipelineCache.h"

#include <algorithm>
#include <array>
#include <glm/glm.hpp>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}

struct DepthHierarchyPush {
    glm::uvec4 extents{0u};
};

} // namespace

DepthHierarchyPass::DepthHierarchyPass(
    Device &device, const RenderResourcePool &resources,
    RenderImageHandle sourceDepth, RenderSamplerHandle sourceDepthSampler,
    DepthHierarchyResources hierarchy,
    DescriptorAllocator &descriptorAllocator,
    DepthHierarchyPrograms programs)
    : device_(&device), sourceDepth_(sourceDepth),
      sourceDepthSampler_(sourceDepthSampler), hierarchy_(hierarchy),
      descriptorAllocator_(&descriptorAllocator) {
    if (!sourceDepth_.valid() || !sourceDepthSampler_.valid() ||
        !hierarchy_.available()) {
        throw std::invalid_argument(
            "DepthHierarchyPass requires valid depth hierarchy resources");
    }
    chains_[static_cast<size_t>(Chain::Combined)] = {
        hierarchy_.combinedMinMax, hierarchy_.combinedOrNearestSampler,
        "Combined", std::move(programs.combinedInit),
        std::move(programs.combinedReduce), {}};
    chains_[static_cast<size_t>(Chain::Nearest)] = {
        hierarchy_.nearestFallback, hierarchy_.combinedOrNearestSampler,
        "Nearest", std::move(programs.nearestInit),
        std::move(programs.nearestReduce), {}};
    chains_[static_cast<size_t>(Chain::Farthest)] = {
        hierarchy_.farthestFallback, hierarchy_.farthestFallbackSampler,
        "Farthest", std::move(programs.farthestInit),
        std::move(programs.farthestReduce), {}};
    createDescriptorSetLayout();
    createDescriptors(resources);
}

DepthHierarchyPass::~DepthHierarchyPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

bool DepthHierarchyPass::chainRequired(
    Chain value, const FrameRenderFeatures &features) const {
    if (hierarchy_.combined())
        return value == Chain::Combined && features.depthHierarchyRequired;
    if (value == Chain::Nearest)
        return features.screenDepthPyramidRequired;
    if (value == Chain::Farthest)
        return features.hiZRequired;
    return false;
}

const DepthHierarchyPass::ChainState &
DepthHierarchyPass::chain(Chain value) const {
    return chains_.at(static_cast<size_t>(value));
}

DepthHierarchyPass::ChainState &DepthHierarchyPass::chain(Chain value) {
    return chains_.at(static_cast<size_t>(value));
}

void DepthHierarchyPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    for (uint32_t chainIndex = 0; chainIndex < chains_.size(); ++chainIndex) {
        const Chain value = static_cast<Chain>(chainIndex);
        const ChainState &state = chain(value);
        if (!state.image.valid() || !chainRequired(value, context.features))
            continue;
        const uint32_t mipCount =
            context.resources.mipLevelCount(state.image);
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            builder.addNode(
                "DepthHierarchy/" + state.name + "/Mip" +
                    std::to_string(mip),
                RgPassType::Compute, RgQueueClass::Compute,
                chainIndex * kLocalNodeStride + mip);
            if (mip == 0) {
                builder.useImage(
                    {sourceDepth_, RenderImageAccess::SampledRead,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
            } else {
                builder.useImage(
                    {state.image, RenderImageAccess::SampledRead,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 1, 0, 1});
            }
            builder.useImage(
                {state.image, RenderImageAccess::StorageWrite,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
                {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1});
        }
    }
}

void DepthHierarchyPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &) {
    const uint32_t chainIndex = localNodeIndex / kLocalNodeStride;
    const uint32_t mip = localNodeIndex % kLocalNodeStride;
    if (chainIndex >= chains_.size())
        return;
    recordMip(context.frame, context.resources,
              static_cast<Chain>(chainIndex), mip);
}

void DepthHierarchyPass::recordMip(
    const RenderFrameContext &frame, const RenderResourcePool &resources,
    Chain value, uint32_t mip) {
    const ChainState &state = chain(value);
    if (!frame.pipelineCache || !state.image.valid() ||
        mip >= resources.mipLevelCount(state.image))
        return;
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/DepthHierarchy/" + state.name +
                       (mip == 0 ? "/Init" : "/Reduce");
    config.computeShaderPath =
        mip == 0 ? state.initShader : state.reduceShader;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(DepthHierarchyPush)}};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = state.sets.at(frame.frameIndex).at(mip);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    const VkExtent2D destination = resources.mipExtent(state.image, mip);
    const VkExtent2D source =
        mip == 0 ? resources.extent(sourceDepth_)
                 : resources.mipExtent(state.image, mip - 1u);
    const DepthHierarchyPush push{{source.width, source.height,
                                   destination.width,
                                   destination.height}};
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, dispatchCount(destination.width),
                  dispatchCount(destination.height), 1);
}

void DepthHierarchyPass::createDescriptorSetLayout() {
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/DepthHierarchy");
}

void DepthHierarchyPass::createDescriptors(
    const RenderResourcePool &resources) {
    for (uint32_t index = 0; index < chains_.size(); ++index) {
        const Chain value = static_cast<Chain>(index);
        const ChainState &state = chain(value);
        if (state.image.valid() && resources.resident(state.image))
            createChainDescriptors(resources, value);
    }
}

void DepthHierarchyPass::createChainDescriptors(
    const RenderResourcePool &resources, Chain value) {
    ChainState &state = chain(value);
    const uint32_t mipCount = resources.mipLevelCount(state.image);
    for (uint32_t frame = 0; frame < state.sets.size(); ++frame) {
        state.sets[frame].resize(mipCount);
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "DepthHierarchy/" + state.name + "/Frame" +
                    std::to_string(frame) + "/Mip" +
                    std::to_string(mip));
            state.sets[frame][mip] = set;
            VkDescriptorImageInfo source{};
            if (mip == 0) {
                source.sampler = resources.sampler(sourceDepthSampler_);
                source.imageView =
                    resources.image(sourceDepth_, frame).imageView();
                source.imageLayout =
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            } else {
                source.sampler = resources.sampler(state.sampler);
                source.imageView =
                    resources.mipView(state.image, frame, mip - 1u);
                source.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            VkDescriptorImageInfo destination{};
            destination.imageView =
                resources.mipView(state.image, frame, mip);
            destination.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0,
                 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source,
                 nullptr, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0,
                 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destination, nullptr,
                 nullptr},
            }};
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
}

void DepthHierarchyPass::freeDescriptors() {
    for (uint32_t index = 0; index < chains_.size(); ++index)
        freeChainDescriptors(static_cast<Chain>(index));
}

void DepthHierarchyPass::freeChainDescriptors(Chain value) {
    ChainState &state = chain(value);
    for (auto &frameSets : state.sets) {
        for (VkDescriptorSet set : frameSets)
            descriptorAllocator_->free(set);
        frameSets.clear();
    }
}

void DepthHierarchyPass::releaseViewportResources() {
    freeDescriptors();
}

void DepthHierarchyPass::onViewportResize(
    const RenderResourcePool &resources) {
    createDescriptors(resources);
}

void DepthHierarchyPass::onResourceResidencyChanged(
    const RenderResourcePool &resources, uint32_t,
    const std::vector<RenderImageHandle> &createdImages) {
    const bool hierarchyCreated = std::any_of(
        chains_.begin(), chains_.end(), [&](const ChainState &state) {
            return state.image.valid() &&
                   std::find(createdImages.begin(), createdImages.end(),
                             state.image) != createdImages.end();
        });
    if (!hierarchyCreated)
        return;
    for (uint32_t index = 0; index < chains_.size(); ++index) {
        const Chain value = static_cast<Chain>(index);
        const ChainState &state = chain(value);
        if (!state.image.valid() ||
            std::find(createdImages.begin(), createdImages.end(),
                      state.image) == createdImages.end()) {
            continue;
        }
        freeChainDescriptors(value);
        createChainDescriptors(resources, value);
    }
}

} // namespace vkr
