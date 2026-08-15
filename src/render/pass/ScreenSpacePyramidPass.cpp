#include "render/pass/ScreenSpacePyramidPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"

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

struct PyramidPush {
    glm::uvec4 extents{0u};
};

} // namespace

ScreenSpacePyramidPass::ScreenSpacePyramidPass(
    Device &device, const RenderResourceRegistry &resources,
    ScreenSpacePyramidKind kind, RenderImageHandle source,
    RenderSamplerHandle sourceSampler, RenderImageHandle alternateSource,
    RenderSamplerHandle alternateSourceSampler, RenderImageHandle pyramid,
    RenderSamplerHandle pyramidSampler,
    DescriptorAllocator &descriptorAllocator, std::string initShaderPath,
    std::string reduceShaderPath)
    : device_(&device), kind_(kind),
      name_(kind == ScreenSpacePyramidKind::NearestDepth
                ? "ScreenDepthPyramid"
                : "SceneColorPyramid"),
      source_(source), sourceSampler_(sourceSampler),
      alternateSource_(alternateSource),
      alternateSourceSampler_(alternateSourceSampler), pyramid_(pyramid),
      pyramidSampler_(pyramidSampler),
      descriptorAllocator_(&descriptorAllocator),
      initShaderPath_(std::move(initShaderPath)),
      reduceShaderPath_(std::move(reduceShaderPath)) {
    if (!source_.valid() || !sourceSampler_.valid() || !pyramid_.valid() ||
        !pyramidSampler_.valid()) {
        throw std::invalid_argument(
            "ScreenSpacePyramidPass requires valid resources");
    }
    createDescriptorSetLayout();
    createDescriptors(resources);
}

ScreenSpacePyramidPass::~ScreenSpacePyramidPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void ScreenSpacePyramidPass::releaseViewportResources() {
    freeDescriptors();
}

void ScreenSpacePyramidPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void ScreenSpacePyramidPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    const uint32_t mipCount = context.resources.mipLevelCount(pyramid_);
    const VkImageLayout sourceLayout =
        kind_ == ScreenSpacePyramidKind::NearestDepth
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        builder.addNode(name_ + "/Mip" + std::to_string(mip),
                        RgPassType::Compute, RgQueueClass::Compute, mip);
        if (mip == 0) {
            if (kind_ == ScreenSpacePyramidKind::SceneColor &&
                context.features.taaActive && alternateSource_.valid()) {
                builder.useImage(
                    {alternateSource_, RenderImageAccess::SampledRead,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            } else {
                builder.useImage({source_, RenderImageAccess::SampledRead,
                                  sourceLayout, sourceLayout});
            }
        } else {
            builder.useImage(
                {pyramid_, RenderImageAccess::SampledRead,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
                {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 1, 0, 1});
        }
        builder.useImage(
            {pyramid_, RenderImageAccess::StorageWrite,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
            {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1});
    }
    builder.addNode(name_ + "/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, mipCount);
    builder.useImage(
        {pyramid_, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

void ScreenSpacePyramidPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &) {
    if (localNodeIndex < context.resources.mipLevelCount(pyramid_))
        recordMip(context.frame, context.resources, localNodeIndex);
}

void ScreenSpacePyramidPass::recordMip(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources, uint32_t mip) {
    if (!frame.pipelineCache || mip >= resources.mipLevelCount(pyramid_))
        return;
    const bool useAlternateSource =
        kind_ == ScreenSpacePyramidKind::SceneColor &&
        frame.features.taaActive && alternateSource_.valid();
    if (mip == 0)
        updateInitialSource(resources, frame.frameIndex,
                            useAlternateSource);
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PyramidPush)};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/" + name_ +
                       (mip == 0 ? "/Init" : "/Reduce");
    config.computeShaderPath = mip == 0 ? initShaderPath_ : reduceShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {pushRange};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = sets_[frame.frameIndex].at(mip);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    const VkExtent2D destination = resources.mipExtent(pyramid_, mip);
    const RenderImageHandle initialSource =
        useAlternateSource ? alternateSource_ : source_;
    const VkExtent2D sourceExtent =
        mip == 0 ? resources.extent(initialSource)
                 : resources.mipExtent(pyramid_, mip - 1u);
    const PyramidPush push{{sourceExtent.width, sourceExtent.height,
                            destination.width, destination.height}};
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, dispatchCount(destination.width),
                  dispatchCount(destination.height), 1);
}

void ScreenSpacePyramidPass::updateInitialSource(
    const RenderResourceRegistry &resources, uint32_t frameIndex,
    bool useAlternateSource) {
    const RenderImageHandle source =
        useAlternateSource ? alternateSource_ : source_;
    const RenderSamplerHandle sampler =
        useAlternateSource ? alternateSourceSampler_ : sourceSampler_;
    VkDescriptorImageInfo info{
        resources.sampler(sampler),
        resources.image(source, frameIndex).imageView(),
        kind_ == ScreenSpacePyramidKind::NearestDepth
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[frameIndex].front();
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0, nullptr);
}

void ScreenSpacePyramidPass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/" + name_);
}

void ScreenSpacePyramidPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    const uint32_t mipCount = resources.mipLevelCount(pyramid_);
    const VkImageLayout initialSourceLayout =
        kind_ == ScreenSpacePyramidKind::NearestDepth
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (uint32_t frame = 0; frame < sets_.size(); ++frame) {
        sets_[frame].resize(mipCount);
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                name_ + "/Frame" + std::to_string(frame) + "/Mip" +
                    std::to_string(mip));
            sets_[frame][mip] = set;
            VkDescriptorImageInfo source{};
            if (mip == 0) {
                source.sampler = resources.sampler(sourceSampler_);
                source.imageView = resources.image(source_, frame).imageView();
                source.imageLayout = initialSourceLayout;
            } else {
                source.sampler = resources.sampler(pyramidSampler_);
                source.imageView = resources.mipView(pyramid_, frame,
                                                     mip - 1u);
                source.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            VkDescriptorImageInfo destination{};
            destination.imageView = resources.mipView(pyramid_, frame, mip);
            destination.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         set, 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         &source, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         &destination, nullptr, nullptr};
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
}

void ScreenSpacePyramidPass::freeDescriptors() {
    for (auto &frameSets : sets_) {
        for (VkDescriptorSet set : frameSets)
            descriptorAllocator_->free(set);
        frameSets.clear();
    }
}

} // namespace vkr
