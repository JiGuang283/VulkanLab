#include "render/pass/CacaoNormalAdapterPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuBarrier.h"
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
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}

} // namespace

CacaoNormalAdapterPass::CacaoNormalAdapterPass(
    Device &device, const RenderResourceRegistry &resources,
    RendererResourceHandles resourceHandles,
    DescriptorAllocator &descriptorAllocator,
    VkDescriptorSetLayout globalDescriptorSetLayout, std::string shaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      shaderPath_(std::move(shaderPath)) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.cacaoDepth.valid() ||
        !resourceHandles_.cacaoViewNormals.valid()) {
        throw std::invalid_argument(
            "CacaoNormalAdapterPass requires surface and CACAO inputs");
    }
    createDescriptorSetLayout();
    createDescriptors(resources);
}

CacaoNormalAdapterPass::~CacaoNormalAdapterPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void CacaoNormalAdapterPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &) const {
    builder.addNode(std::string(name()), RgPassType::Compute,
                    RgQueueClass::Graphics);
    builder.useImage({resourceHandles_.surfaceDepth,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
    builder.useImage({resourceHandles_.surfaceNormalRoughness,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resourceHandles_.cacaoDepth,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resourceHandles_.cacaoViewNormals,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

void CacaoNormalAdapterPass::releaseViewportResources() {
    freeDescriptors();
}

void CacaoNormalAdapterPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void CacaoNormalAdapterPass::execute(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources, const VisibilityFrame &) {
    if (!frame.features.cacaoRequired || !frame.pipelineCache)
        return;

    VKL_PROFILE_ZONE("Record CACAO Normal Adapter");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                         "CACAO Normal Adapter");
    const uint32_t frameIndex = frame.frameIndex;

    ComputePipelineConfig config{};
    config.debugName = "Pipeline/ScreenSpace/CACAO/NormalAdapter";
    config.computeShaderPath = shaderPath_;
    config.descriptorLayouts = {globalDescriptorSetLayout_,
                                descriptorSetLayout_};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 2> sets = {
        frame.globalDescriptorSet, descriptorSets_[frameIndex]};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    const VkExtent2D extent =
        resources.extent(resourceHandles_.cacaoViewNormals);
    vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                  dispatchCount(extent.height), 1);
}

void CacaoNormalAdapterPass::createDescriptorSetLayout() {
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
        VkDescriptorSetLayoutBinding{
            0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{
            1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/CACAO/NormalAdapter");
}

void CacaoNormalAdapterPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    const VkSampler sampler =
        resources.sampler(resourceHandles_.surfaceDataSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}},
            "CACAO/NormalAdapter/Frame" + std::to_string(frame));
        const std::array<VkDescriptorImageInfo, 4> infos = {
            VkDescriptorImageInfo{
                resources.sampler(resourceHandles_.surfaceDepthSampler),
                resources.image(resourceHandles_.surfaceDepth, frame)
                    .imageView(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{
                sampler,
                resources
                    .image(resourceHandles_.surfaceNormalRoughness, frame)
                    .imageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{
                VK_NULL_HANDLE,
                resources.image(resourceHandles_.cacaoDepth, frame)
                    .imageView(),
                VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{
                VK_NULL_HANDLE,
                resources.image(resourceHandles_.cacaoViewNormals, frame)
                    .imageView(),
                VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = descriptorSets_[frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                binding < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[binding].pImageInfo = &infos[binding];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void CacaoNormalAdapterPass::freeDescriptors() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

} // namespace vkr
