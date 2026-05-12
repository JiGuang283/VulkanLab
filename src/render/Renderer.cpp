#include "Renderer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "core/VulkanCheck.h"
#include "core/Pipeline.h"
#include "Material.h"
#include "Mesh.h"
#include "RenderQueue.h"

#include <array>
#include <glm/glm.hpp>

namespace vkr {

namespace {
struct GpuPushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveMetallic; // xyz=emissive, w=metallic
    glm::vec4 roughnessAlpha;   // x=roughness, y=alphaCutoff
    glm::vec4 reserved;         // padding to reach 128B (Vulkan min)
};
static_assert(sizeof(GpuPushBlock) == 128, "push block must be 128B");
} // namespace

Renderer::Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
                   DescriptorAllocator &descriptorAllocator,
                   VkDeviceSize uniformBufferSize)
    : device_(&device), swapChain_(&swapChain), frameSync_(&frameSync),
      descriptorAllocator_(&descriptorAllocator),
      uniformBufferSize_(uniformBufferSize) {
    createRenderPass();
    createColorResources();
    createDepthResources();
    createFramebuffers();
    createUniformBuffers();
    createGlobalDescriptorSetLayout();
    createGlobalDescriptorSets();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());

    cleanupSwapChainResources();
    uniformBuffers_.clear();

    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 globalDescriptorSetLayout_, nullptr);
    vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

// ---- RenderPass 辅助 ----

void Renderer::beginRenderPass(VkCommandBuffer cmd, uint32_t imageIndex) {
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChain_->extent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 自动设置全屏 viewport & scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapChain_->extent().width);
    viewport.height = static_cast<float>(swapChain_->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapChain_->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::endRenderPass(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

void Renderer::drawQueue(VkCommandBuffer cmd, uint32_t frameIndex,
                         Pipeline &pipeline, const RenderQueue &queue) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    for (const auto &command : queue.opaque()) {
        if (!command.mesh || !command.material)
            continue;

        command.material->bindDescriptors(cmd, pipeline.layout(), frameIndex);

        const auto  &p = command.material->params();
        GpuPushBlock blk{};
        blk.model = command.world;
        blk.baseColorFactor = p.baseColorFactor;
        blk.emissiveMetallic = glm::vec4(p.emissiveFactor, p.metallicFactor);
        blk.roughnessAlpha =
            glm::vec4(p.roughnessFactor, p.alphaCutoff, 0.0f, 0.0f);

        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GpuPushBlock), &blk);
        command.mesh->bind(cmd);
        command.mesh->draw(cmd);
    }
}

// ---- 单次命令辅助（委托给 FrameSync）----
// 已移至 FrameSync

// ---- 交换链重建 ----

void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());

    cleanupSwapChainResources();

    swapChain_->recreate();
    createColorResources();
    createDepthResources();
    createFramebuffers();
}

void Renderer::cleanupSwapChainResources() {
    VkDevice d = device_->logicalDevice();

    for (auto framebuffer : framebuffers_) {
        vkDestroyFramebuffer(d, framebuffer, nullptr);
    }
    framebuffers_.clear();

    colorImage_.reset();
    depthImage_.reset();
}

// ---- RenderPass ----

void Renderer::createRenderPass() {
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = swapChain_->imageFormat();
    colorAttachment.samples = device_->msaaSamples();
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = findDepthFormat();
    depthAttachment.samples = device_->msaaSamples();
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription colorAttachmentResolve{};
    colorAttachmentResolve.format = swapChain_->imageFormat();
    colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentResolveRef{};
    colorAttachmentResolveRef.attachment = 2;
    colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;
    subpass.pResolveAttachments = &colorAttachmentResolveRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 3> attachments = {
        colorAttachment, depthAttachment, colorAttachmentResolve};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &renderPassInfo,
                                nullptr, &renderPass_));
}

// ---- Framebuffers ----

void Renderer::createFramebuffers() {
    framebuffers_.resize(swapChain_->imageViews().size());

    for (size_t i = 0; i < swapChain_->imageViews().size(); i++) {
        std::array<VkImageView, 3> attachments = {colorImage_->imageView(),
                                                  depthImage_->imageView(),
                                                  swapChain_->imageViews()[i]};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount =
            static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapChain_->extent().width;
        framebufferInfo.height = swapChain_->extent().height;
        framebufferInfo.layers = 1;

        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &framebufferInfo,
                                     nullptr, &framebuffers_[i]));
    }
}

// ---- Color / Depth resources ----

void Renderer::createColorResources() {
    VkFormat colorFormat = swapChain_->imageFormat();

    colorImage_ = std::make_unique<Image>(
        *device_, swapChain_->extent().width, swapChain_->extent().height, 1,
        device_->msaaSamples(), colorFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    colorImage_->createView(colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
}

void Renderer::createDepthResources() {
    VkFormat depthFormat = findDepthFormat();

    depthImage_ = std::make_unique<Image>(
        *device_, swapChain_->extent().width, swapChain_->extent().height, 1,
        device_->msaaSamples(), depthFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    depthImage_->createView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);

    // Transition depth image layout inline
    VkCommandBuffer cmd = frameSync_->beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImage_->handle();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        depthFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
        barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    frameSync_->endSingleTimeCommands(cmd);
}

// ---- Format helpers ----

VkFormat Renderer::findSupportedFormat(const std::vector<VkFormat> &candidates,
                                       VkImageTiling                tiling,
                                       VkFormatFeatureFlags         features) {
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                            &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        } else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
                   (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error("failed to find supported format!");
}

VkFormat Renderer::findDepthFormat() {
    return findSupportedFormat({VK_FORMAT_D32_SFLOAT,
                                VK_FORMAT_D32_SFLOAT_S8_UINT,
                                VK_FORMAT_D24_UNORM_S8_UINT},
                               VK_IMAGE_TILING_OPTIMAL,
                               VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

// ---- Uniform Buffers ----

void Renderer::createUniformBuffers() {
    if (uniformBufferSize_ == 0)
        return;
    uniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto &buf : uniformBuffers_) {
        buf = std::make_unique<Buffer>(
            *device_, uniformBufferSize_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        buf->map();
    }
}

void Renderer::createGlobalDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboLayoutBinding;

    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo,
                                         nullptr,
                                         &globalDescriptorSetLayout_));
}

void Renderer::createGlobalDescriptorSets() {
    if (uniformBuffers_.empty())
        return;

    globalDescriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto &set : globalDescriptorSets_)
        set = descriptorAllocator_->allocate(globalDescriptorSetLayout_);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i]->handle();
        bufferInfo.offset = 0;
        bufferInfo.range = uniformBufferSize_;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = globalDescriptorSets_[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &descriptorWrite, 0,
                               nullptr);
    }
}

void *Renderer::mappedUniformBuffer(uint32_t frameIndex) const {
    return uniformBuffers_[frameIndex]->mappedData();
}

void Renderer::bindGlobalDescriptors(VkCommandBuffer cmd,
                                     VkPipelineLayout layout,
                                     uint32_t frameIndex) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &globalDescriptorSets_[frameIndex], 0, nullptr);
}

} // namespace vkr
