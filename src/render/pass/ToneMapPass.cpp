#include "ToneMapPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/SwapChain.h"
#include "core/VulkanCheck.h"
#include "render/FrameRenderTargets.h"
#include "render/GuiSystem.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderSettings.h"
#include "render/ShaderVariant.h"

#include <array>
#include <utility>

namespace vkr {

namespace {

struct ToneMapPushConstants {
    float exposureEv = 0.0f;
    uint32_t toneMapper = 0;
    uint32_t encodeGamma = 0;
    uint32_t applyExposure = 0;
};

bool usesPbrToneMapping(ShaderVariantId id) {
    return id == ShaderVariantId::PbrLiteForward ||
           id == ShaderVariantId::PbrLiteNormalMapped;
}

uint32_t toneMapperValue(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::PassThrough:
        return 0;
    case ToneMapper::Reinhard:
        return 1;
    case ToneMapper::Aces:
        return 2;
    }
    return 2;
}

bool isSrgbFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_SRGB;
}

} // namespace

ToneMapPass::ToneMapPass(Device &device, SwapChain &swapChain,
                         FrameRenderTargets &targets,
                         DescriptorAllocator &descriptorAllocator,
                         std::string fullscreenVertPath,
                         std::string toneMapFragPath)
    : device_(&device), swapChain_(&swapChain), targets_(&targets),
      descriptorAllocator_(&descriptorAllocator),
      fullscreenVertPath_(std::move(fullscreenVertPath)),
      toneMapFragPath_(std::move(toneMapFragPath)) {
    createRenderPass();
    createDescriptors();
    createFramebuffers();
}

ToneMapPass::~ToneMapPass() {
    destroyFramebuffers();
    for (VkDescriptorSet set : sourceDescriptorSets_)
        descriptorAllocator_->free(set);
    if (sourceDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     sourceDescriptorSetLayout_, nullptr);
    }
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

void ToneMapPass::releaseSwapChainResources() {
    destroyFramebuffers();
}

void ToneMapPass::onResize(const SwapChain &) {
    updateDescriptors();
    createFramebuffers();
}

void ToneMapPass::execute(const RenderFrameContext &frame,
                          const RenderQueue &) {
    if (!frame.pipelineCache || !frame.settings || !frame.shaderVariant)
        return;

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = framebuffers_.at(frame.imageIndex);
    beginInfo.renderArea = {{0, 0}, frame.extent};
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.extent.width);
    viewport.height = static_cast<float>(frame.extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    PipelineConfig config =
        PipelineConfigBuilder{}
            .shaders(fullscreenVertPath_, toneMapFragPath_)
            .vertexLayout(VertexLayout{})
            .rasterization(VK_CULL_MODE_NONE,
                           VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .depth(false, false)
            .blending(false)
            .msaa(VK_SAMPLE_COUNT_1_BIT)
            .descriptorLayout(sourceDescriptorSetLayout_)
            .pushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ToneMapPushConstants)})
            .build();

    PipelineKey key{};
    key.pass = PassId::ToneMap;
    key.renderPass = renderPass_;
    key.samples = VK_SAMPLE_COUNT_1_BIT;
    Pipeline &pipeline = frame.pipelineCache->getOrCreate(key, config);
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline.handle());
    const VkDescriptorSet sourceSet =
        sourceDescriptorSets_.at(frame.frameIndex);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout(), 0, 1, &sourceSet, 0, nullptr);

    ToneMapPushConstants push{};
    if (usesPbrToneMapping(frame.shaderVariant->id)) {
        push.exposureEv = frame.settings->exposureEv;
        push.toneMapper = toneMapperValue(frame.settings->toneMapper);
        push.applyExposure = 1;
    }
    push.encodeGamma = isSrgbFormat(swapChain_->imageFormat()) ? 0u : 1u;
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);

    if (frame.gui)
        frame.gui->render(frame.cmd);
    vkCmdEndRenderPass(frame.cmd);
}

void ToneMapPass::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapChain_->imageFormat();
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                &renderPass_));
}

void ToneMapPass::createFramebuffers() {
    framebuffers_.resize(swapChain_->imageViews().size());
    for (size_t index = 0; index < framebuffers_.size(); ++index) {
        const VkImageView attachment = swapChain_->imageViews()[index];
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.width = swapChain_->extent().width;
        info.height = swapChain_->extent().height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info, nullptr,
                                     &framebuffers_[index]));
    }
}

void ToneMapPass::destroyFramebuffers() {
    for (VkFramebuffer framebuffer : framebuffers_)
        vkDestroyFramebuffer(device_->logicalDevice(), framebuffer, nullptr);
    framebuffers_.clear();
}

void ToneMapPass::createDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &sourceDescriptorSetLayout_));
    for (VkDescriptorSet &set : sourceDescriptorSets_) {
        set = descriptorAllocator_->allocate(
            sourceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}});
    }
    updateDescriptors();
}

void ToneMapPass::updateDescriptors() {
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = targets_->hdrSampler();
        imageInfo.imageView = targets_->frame(frameIndex).hdrColor->imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sourceDescriptorSets_[frameIndex];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0,
                               nullptr);
    }
}

} // namespace vkr
