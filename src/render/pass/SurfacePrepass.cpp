#include "render/pass/SurfacePrepass.h"

#include "core/Buffer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/FrameGpuData.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialSystem.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/Visibility.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {

namespace {

constexpr uint32_t kInitialHistoryCapacity = 256;

uint32_t nextHistoryCapacity(uint32_t required) {
    uint32_t capacity = kInitialHistoryCapacity;
    while (capacity < required)
        capacity *= 2u;
    return capacity;
}

} // namespace

struct SurfacePrepass::FrameStorage {
    std::unique_ptr<Buffer> frameUbo;
    std::unique_ptr<Buffer> history;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t historyCapacity = 0;
};

SurfacePrepass::SurfacePrepass(
    Device &device, const RenderResourceRegistry &,
    RendererResourceHandles resourceHandles,
    DescriptorAllocator &descriptorAllocator,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    std::string vertexShaderPath,
    std::string opaqueFragmentShaderPath,
    std::string maskFragmentShaderPath)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      resourceHandles_(resourceHandles),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      vertexShaderPath_(std::move(vertexShaderPath)),
      opaqueFragmentShaderPath_(std::move(opaqueFragmentShaderPath)),
      maskFragmentShaderPath_(std::move(maskFragmentShaderPath)) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.surfaceMotion.valid()) {
        throw std::invalid_argument(
            "SurfacePrepass requires all surface attachments");
    }
    createDescriptorSetLayout();
    createFrameStorage();
}

SurfacePrepass::~SurfacePrepass() {
    for (auto &frame : frames_) {
        if (frame && frame->descriptorSet != VK_NULL_HANDLE)
            descriptorAllocator_->free(frame->descriptorSet);
    }
    for (auto &frame : frames_)
        frame.reset();
    if (surfaceDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     surfaceDescriptorSetLayout_, nullptr);
    }
}

void SurfacePrepass::setup(RenderGraphBuilder &builder,
                           const RenderGraphBuildContext &) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    builder.addColorAttachment(
        resourceHandles_.surfaceNormalRoughness,
        RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        {{0.5f, 0.5f, 1.0f, 0.0f}});
    builder.addColorAttachment(
        resourceHandles_.surfaceMotion,
        RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        {{0.0f, 0.0f, 0.0f, 0.0f}});
    if (resourceHandles_.surfaceAlbedoMetallic.valid()) {
        builder.addColorAttachment(
            resourceHandles_.surfaceAlbedoMetallic,
            RenderImageAccess::ColorAttachmentWrite,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {{0.0f, 0.0f, 0.0f, 0.0f}});
    }
    builder.addDepthAttachment(
        resourceHandles_.surfaceDepth,
        RenderImageAccess::DepthAttachmentWrite,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
}

void SurfacePrepass::recordNode(
    RenderGraphPassContext &context, uint32_t,
    const VisibilityFrame &visibility) {
    const VkExtent2D extent =
        context.resources.extent(resourceHandles_.surfaceDepth);
    prepareFrame(context.frame.frameIndex, visibility, extent);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(context.frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(context.frame.cmd, 0, 1, &scissor);
    draw(context.frame, context.resources, visibility);
}

void SurfacePrepass::execute(const RenderFrameContext &frame,
                             const RenderResourceRegistry &resources,
                             const VisibilityFrame &visibility) {
    if (!frame.features.surfaceDataRequired || !frame.pipelineCache)
        return;

    (void)frame;
    (void)resources;
    (void)visibility;
}

uint32_t SurfacePrepass::historyCapacity(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->historyCapacity;
}

uint64_t SurfacePrepass::allocatedBytes() const {
    uint64_t bytes = 0;
    for (const auto &frame : frames_) {
        bytes += sizeof(SurfaceFrameUbo);
        bytes += static_cast<uint64_t>(frame->historyCapacity) *
                 sizeof(GpuRenderItemHistory);
    }
    return bytes;
}

void SurfacePrepass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &surfaceDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, surfaceDescriptorSetLayout_,
        "DescriptorLayout/SurfaceFrame");
}

void SurfacePrepass::createFrameStorage() {
    constexpr VmaAllocationCreateFlags mappedFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    for (uint32_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
        auto storage = std::make_unique<FrameStorage>();
        storage->frameUbo = std::make_unique<Buffer>(
            *device_, sizeof(SurfaceFrameUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mappedFlags,
            "Frame/" + std::to_string(frameIndex) + "/SurfaceFrameUbo");
        storage->frameUbo->map();
        storage->historyCapacity = kInitialHistoryCapacity;
        storage->history = std::make_unique<Buffer>(
            *device_, sizeof(GpuRenderItemHistory) *
                          storage->historyCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mappedFlags,
            "Frame/" + std::to_string(frameIndex) +
                "/SurfaceHistory/Capacity" +
                std::to_string(storage->historyCapacity));
        storage->history->map();
        storage->descriptorSet = descriptorAllocator_->allocate(
            surfaceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
            "Frame/" + std::to_string(frameIndex) +
                "/SurfaceDescriptorSet");
        frames_[frameIndex] = std::move(storage);
        updateDescriptor(frameIndex);
    }
}

void SurfacePrepass::updateDescriptor(uint32_t frameIndex) {
    const FrameStorage &storage = *frames_.at(frameIndex);
    const VkDescriptorBufferInfo uboInfo{
        storage.frameUbo->handle(), 0, sizeof(SurfaceFrameUbo)};
    const VkDescriptorBufferInfo historyInfo{
        storage.history->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = storage.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &uboInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = storage.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &historyInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void SurfacePrepass::ensureHistoryCapacity(uint32_t frameIndex,
                                           uint32_t required) {
    FrameStorage &storage = *frames_.at(frameIndex);
    if (required <= storage.historyCapacity)
        return;
    const uint32_t capacity = nextHistoryCapacity(required);
    constexpr VmaAllocationCreateFlags mappedFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    storage.history = std::make_unique<Buffer>(
        *device_, sizeof(GpuRenderItemHistory) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        mappedFlags,
        "Frame/" + std::to_string(frameIndex) +
            "/SurfaceHistory/Capacity" + std::to_string(capacity));
    storage.history->map();
    storage.historyCapacity = capacity;
    updateDescriptor(frameIndex);
}

void SurfacePrepass::prepareFrame(uint32_t frameIndex,
                                  const VisibilityFrame &visibility,
                                  VkExtent2D extent) {
    ensureHistoryCapacity(
        frameIndex,
        std::max(1u, static_cast<uint32_t>(visibility.items.size())));
    FrameStorage &storage = *frames_.at(frameIndex);

    SurfaceFrameUbo frameUbo{};
    frameUbo.previousViewProjection =
        visibility.history.previousViewProjection;
    frameUbo.viewportSizeInvSize = {
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        extent.width > 0 ? 1.0f / static_cast<float>(extent.width) : 0.0f,
        extent.height > 0 ? 1.0f / static_cast<float>(extent.height) : 0.0f};
    frameUbo.params.x = visibility.history.globalValid ? 1u : 0u;
    frameUbo.params.y =
        static_cast<uint32_t>(visibility.history.historyGeneration);
    std::memcpy(storage.frameUbo->mappedData(), &frameUbo,
                sizeof(frameUbo));

    auto *history = static_cast<GpuRenderItemHistory *>(
        storage.history->mappedData());
    for (uint32_t index = 0; index < visibility.items.size(); ++index) {
        history[index].previousWorld = visibility.items[index].previousWorld;
        history[index].params =
            glm::uvec4(visibility.items[index].historyValid ? 1u : 0u,
                       0u, 0u, 0u);
    }
}

void SurfacePrepass::draw(const RenderFrameContext &frame,
                          const RenderResourceRegistry &resources,
                          const VisibilityFrame &visibility) {
    Pipeline *boundPipeline = nullptr;
    const MaterialInstance *boundMaterial = nullptr;
    for (RenderItemIndex itemIndex : visibility.cameraOpaque) {
        const RenderItem &item = visibility.items.at(itemIndex);
        if (!item.mesh || !item.material)
            continue;

        const MaterialParams &params = item.material->params();
        const bool alphaMasked = params.alphaMode == AlphaMode::Mask;
        const MaterialTemplate &materialTemplate =
            item.material->materialTemplate();
        const VkCullModeFlags cullMode =
            params.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;

        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(vertexShaderPath_,
                         alphaMasked ? maskFragmentShaderPath_
                                     : opaqueFragmentShaderPath_)
                .defaultVertexLayout()
                .rasterization(cullMode,
                               materialTemplate.pipelineConfig().frontFace)
                .depth(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                .colorAttachmentCount(
                    resourceHandles_.surfaceAlbedoMetallic.valid() ? 3u : 2u)
                .blending(false)
                .msaa(VK_SAMPLE_COUNT_1_BIT)
                .descriptorLayout(globalDescriptorSetLayout_)
                .descriptorLayout(materialTemplate.descriptorSetLayout())
                .descriptorLayout(surfaceDescriptorSetLayout_)
                .pushConstant({VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(GpuPushBlock)})
                .build();
        config.debugName =
            "Pipeline/SurfacePrepass/" +
            std::string(alphaMasked ? "Mask" : "Opaque") + "/" +
            (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");
        PipelineRenderingSignature signature{};
        signature.colorAttachmentFormats = {
            resources.description(resourceHandles_.surfaceNormalRoughness)
                .format,
            resources.description(resourceHandles_.surfaceMotion).format};
        if (resourceHandles_.surfaceAlbedoMetallic.valid()) {
            signature.colorAttachmentFormats.push_back(
                resources.description(resourceHandles_.surfaceAlbedoMetallic)
                    .format);
        }
        signature.depthAttachmentFormat =
            resources.description(resourceHandles_.surfaceDepth).format;
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            std::move(signature), std::move(config));
        if (boundPipeline != &pipeline) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
            const std::array<VkDescriptorSet, 2> frameSets{
                frame.globalDescriptorSet,
                frames_.at(frame.frameIndex)->descriptorSet};
            vkCmdBindDescriptorSets(frame.cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline.layout(), 0, 1,
                                    &frameSets[0], 0, nullptr);
            vkCmdBindDescriptorSets(frame.cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline.layout(), 2, 1,
                                    &frameSets[1], 0, nullptr);
            frame.materialSystem->bindGlobal(frame.cmd, pipeline.layout());
            boundPipeline = &pipeline;
            boundMaterial = nullptr;
        }
        if (frame.materialSystem->activeMode() ==
                MaterialBindingMode::Legacy &&
            boundMaterial != item.material) {
            item.material->bindDescriptors(frame.cmd, pipeline.layout(),
                                           frame.frameIndex);
            boundMaterial = item.material;
        }

        GpuPushBlock block{};
        block.model = item.world;
        block.indices =
            glm::uvec4(item.materialIndex, itemIndex, 0u, 0u);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(block), &block);
        item.mesh->bind(frame.cmd);
        item.mesh->draw(frame.cmd, itemIndex);
    }
}

} // namespace vkr
