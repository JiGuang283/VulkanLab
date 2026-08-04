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
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/Visibility.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

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
    Device &device, const RenderResourceRegistry &resources,
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
    createRenderPass(resources);
    createFramebuffers(resources);
}

SurfacePrepass::~SurfacePrepass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
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

std::vector<RenderImageUsage> SurfacePrepass::resourceUsages() const {
    return {
        {resourceHandles_.surfaceNormalRoughness,
         RenderImageAccess::ColorAttachmentWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceMotion,
         RenderImageAccess::ColorAttachmentWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceDepth,
         RenderImageAccess::DepthAttachmentWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
}

void SurfacePrepass::releaseViewportResources() {
    destroyFramebuffers();
}

void SurfacePrepass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void SurfacePrepass::execute(const RenderFrameContext &frame,
                             const RenderResourceRegistry &resources,
                             const VisibilityFrame &visibility) {
    if (!frame.features.surfaceDataRequired || !frame.pipelineCache)
        return;

    VKL_PROFILE_ZONE("Record SurfacePrepass");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "SurfacePrepass");
    const VkExtent2D extent =
        resources.extent(resourceHandles_.surfaceDepth);
    prepareFrame(frame.frameIndex, visibility, extent);

    std::array<VkClearValue, 3> clears{};
    clears[0].color = {{0.5f, 0.5f, 1.0f, 0.0f}};
    clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[2].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass_;
    begin.framebuffer = framebuffers_.at(frame.frameIndex);
    begin.renderArea = {{0, 0}, extent};
    begin.clearValueCount = static_cast<uint32_t>(clears.size());
    begin.pClearValues = clears.data();
    vkCmdBeginRenderPass(frame.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    draw(frame, visibility);
    vkCmdEndRenderPass(frame.cmd);
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
                          const VisibilityFrame &visibility) {
    Pipeline *boundPipeline = nullptr;
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
                .colorAttachmentCount(2)
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
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            renderPass_, std::move(config));
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
            boundPipeline = &pipeline;
        }
        item.material->bindDescriptors(frame.cmd, pipeline.layout(),
                                       frame.frameIndex);

        GpuPushBlock block{};
        block.model = item.world;
        block.baseColorFactor = params.baseColorFactor;
        block.roughnessAlpha.x = params.roughnessFactor;
        block.roughnessAlpha.y = params.alphaCutoff;
        block.reserved.z = params.normalScale;
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(block), &block);
        item.mesh->bind(frame.cmd);
        item.mesh->draw(frame.cmd, itemIndex);
    }
}

void SurfacePrepass::createRenderPass(
    const RenderResourceRegistry &resources) {
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0].format = resources.description(
        resourceHandles_.surfaceNormalRoughness).format;
    attachments[1].format =
        resources.description(resourceHandles_.surfaceMotion).format;
    for (uint32_t index = 0; index < 2; ++index) {
        attachments[index].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[index].finalLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    attachments[2].format =
        resources.description(resourceHandles_.surfaceDepth).format;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    const std::array<VkAttachmentReference, 2> colorRefs{{
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    const VkAttachmentReference depthRef{
        2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount =
        static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                &renderPass_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_RENDER_PASS,
                                        renderPass_,
                                        "RenderPass/SurfacePrepass");
}

void SurfacePrepass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const VkExtent2D extent = resources.extent(resourceHandles_.surfaceDepth);
    for (uint32_t frame = 0; frame < framebuffers_.size(); ++frame) {
        const std::array<VkImageView, 3> attachments{
            resources.image(resourceHandles_.surfaceNormalRoughness, frame)
                .imageView(),
            resources.image(resourceHandles_.surfaceMotion, frame)
                .imageView(),
            resources.image(resourceHandles_.surfaceDepth, frame)
                .imageView()};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info,
                                     nullptr, &framebuffers_[frame]));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[frame],
            "Framebuffer/SurfacePrepass/Frame" + std::to_string(frame));
    }
}

void SurfacePrepass::destroyFramebuffers() {
    for (VkFramebuffer &framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
        }
        framebuffer = VK_NULL_HANDLE;
    }
}

} // namespace vkr
