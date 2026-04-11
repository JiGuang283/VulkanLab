#include "Renderer.h"
#include "core/Device.h"
#include "core/SwapChain.h"

#include <GLFW/glfw3.h>
#include <array>
#include <stdexcept>

#include "vulkan_utils.h"

namespace vkr {

Renderer::Renderer(Device &device, SwapChain &swapChain,
                   VkDeviceSize uniformBufferSize)
    : device_(&device), swapChain_(&swapChain),
      uniformBufferSize_(uniformBufferSize) {
    createCommandPool();
    createRenderPass();
    createColorResources();
    createDepthResources();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
    createUniformBuffers();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());

    cleanupSwapChainResources();

    VkDevice d = device_->logicalDevice();
    for (auto &f : frames_) {
        vkDestroySemaphore(d, f.imageAvailable, nullptr);
        vkDestroySemaphore(d, f.renderFinished, nullptr);
        vkDestroyFence(d, f.inFlight, nullptr);
        f.uniformBuffer.reset();
    }

    vkDestroyRenderPass(d, renderPass_, nullptr);
    vkDestroyCommandPool(d, commandPool_, nullptr);
}

// ---- 帧循环 ----

VkCommandBuffer Renderer::beginFrame() {
    // 窗口最小化时 extent 为 {0,0}，直接跳过
    VkExtent2D ext = swapChain_->extent();
    if (ext.width == 0 || ext.height == 0) {
        return VK_NULL_HANDLE;
    }

    VkDevice d = device_->logicalDevice();

    vkWaitForFences(d, 1, &frames_[currentFrame_].inFlight, VK_TRUE,
                    UINT64_MAX);

    VkResult result =
        vkAcquireNextImageKHR(d, swapChain_->handle(), UINT64_MAX,
                              frames_[currentFrame_].imageAvailable,
                              VK_NULL_HANDLE, &currentImageIndex_);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return VK_NULL_HANDLE;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    vkResetFences(d, 1, &frames_[currentFrame_].inFlight);

    vkResetCommandBuffer(frames_[currentFrame_].commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(frames_[currentFrame_].commandBuffer,
                             &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    frameInProgress_ = true;
    return frames_[currentFrame_].commandBuffer;
}

void Renderer::endFrame() {
    VkDevice d = device_->logicalDevice();

    if (vkEndCommandBuffer(frames_[currentFrame_].commandBuffer) !=
        VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer!");
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {frames_[currentFrame_].imageAvailable};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frames_[currentFrame_].commandBuffer;

    VkSemaphore signalSemaphores[] = {frames_[currentFrame_].renderFinished};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo,
                      frames_[currentFrame_].inFlight) != VK_SUCCESS) {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapChain_->handle()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &currentImageIndex_;

    VkResult result = vkQueuePresentKHR(device_->presentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebufferResized_) {
        framebufferResized_ = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    frameInProgress_ = false;
    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

// ---- RenderPass 辅助 ----

void Renderer::beginRenderPass(VkCommandBuffer cmd) {
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = framebuffers_[currentImageIndex_];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChain_->extent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void Renderer::endRenderPass(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

// ---- 单次命令辅助 ----

VkCommandBuffer Renderer::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo,
                             &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void Renderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphicsQueue());

    vkFreeCommandBuffers(device_->logicalDevice(), commandPool_, 1,
                         &commandBuffer);
}

void Renderer::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmd = beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copyRegion);

    endSingleTimeCommands(cmd);
}

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

    if (vkCreateRenderPass(device_->logicalDevice(), &renderPassInfo, nullptr,
                           &renderPass_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }
}

// ---- CommandPool / CommandBuffers ----

void Renderer::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = device_->queueFamilies();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (vkCreateCommandPool(device_->logicalDevice(), &poolInfo, nullptr,
                            &commandPool_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create command pool!");
    }
}

void Renderer::createCommandBuffers() {
    std::vector<VkCommandBuffer> buffers(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    if (vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo,
                                 buffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffers!");
    }
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        frames_[i].commandBuffer = buffers[i];
    }
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

        if (vkCreateFramebuffer(device_->logicalDevice(), &framebufferInfo,
                                nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create framebuffer!");
        }
    }
}

// ---- 同步对象 ----

void Renderer::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice d = device_->logicalDevice();
    for (auto &f : frames_) {
        if (vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable) !=
                VK_SUCCESS ||
            vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.renderFinished) !=
                VK_SUCCESS ||
            vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS) {
            throw std::runtime_error(
                "failed to create synchronization objects for a frame!");
        }
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
    VkCommandBuffer cmd = beginSingleTimeCommands();

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

    endSingleTimeCommands(cmd);
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
    for (auto &f : frames_) {
        f.uniformBuffer = std::make_unique<Buffer>(
            *device_, uniformBufferSize_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.uniformBuffer->map();
    }
}

void *Renderer::mappedUniformBuffer(uint32_t frameIndex) const {
    return frames_[frameIndex].uniformBuffer->mappedData();
}

VkBuffer Renderer::uniformBufferHandle(uint32_t frameIndex) const {
    return frames_[frameIndex].uniformBuffer->handle();
}

} // namespace vkr
