#include "FrameSync.h"
#include "Device.h"
#include "GpuDebugUtils.h"
#include "SwapChain.h"
#include "VulkanCheck.h"
#include "diagnostics/Profiling.h"

#include <stdexcept>
#include <string>

namespace vkr {

FrameSync::FrameSync(Device &device, SwapChain &swapChain)
    : device_(&device), swapChain_(&swapChain) {
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
}

FrameSync::~FrameSync() {
    vkDeviceWaitIdle(device_->logicalDevice());

    VkDevice d = device_->logicalDevice();
    for (auto &f : frames_) {
        vkDestroySemaphore(d, f.imageAvailable, nullptr);
        vkDestroyFence(d, f.inFlight, nullptr);
    }
    for (auto sem : renderFinished_) {
        vkDestroySemaphore(d, sem, nullptr);
    }
    vkDestroyCommandPool(d, commandPool_, nullptr);
}

// ---- 帧循环 ----

std::optional<FrameSync::FrameContext> FrameSync::beginFrame() {
    VkExtent2D ext = swapChain_->extent();
    if (ext.width == 0 || ext.height == 0)
        return std::nullopt;

    VkDevice d = device_->logicalDevice();

    {
        VKL_PROFILE_ZONE("Frame Fence Wait");
        VK_CHECK(vkWaitForFences(d, 1, &frames_[currentFrame_].inFlight,
                                 VK_TRUE, UINT64_MAX));
    }
    submissionSerials_.completeFrameSlot(currentFrame_);

    uint32_t imageIndex;
    VkResult result = VK_SUCCESS;
    {
        VKL_PROFILE_ZONE("Swapchain Acquire");
        result = vkAcquireNextImageKHR(
            d, swapChain_->handle(), UINT64_MAX,
            frames_[currentFrame_].imageAvailable, VK_NULL_HANDLE,
            &imageIndex);
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapChainOutOfDate_ = true;
        return std::nullopt;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    vkResetFences(d, 1, &frames_[currentFrame_].inFlight);
    vkResetCommandBuffer(frames_[currentFrame_].commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(
        vkBeginCommandBuffer(frames_[currentFrame_].commandBuffer, &beginInfo));

    return FrameContext{frames_[currentFrame_].commandBuffer, currentFrame_,
                        imageIndex};
}

uint64_t FrameSync::endFrame(const FrameContext &ctx) {
    VKL_PROFILE_ZONE("Frame Submit And Present");
    VK_CHECK(vkEndCommandBuffer(ctx.cmd));

    // Submit
    VkSemaphore          waitSems[] = {frames_[ctx.frameIndex].imageAvailable};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSems[] = {renderFinished_[ctx.imageIndex]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSems;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &ctx.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSems;

    {
        VKL_PROFILE_ZONE("Graphics Queue Submit");
        VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo,
                               frames_[ctx.frameIndex].inFlight));
    }
    const uint64_t submissionSerial =
        submissionSerials_.recordSubmission(ctx.frameIndex);

    // Present
    VkSwapchainKHR   swapChains[] = {swapChain_->handle()};
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSems;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &ctx.imageIndex;

    VkResult result = VK_SUCCESS;
    {
        VKL_PROFILE_ZONE("Queue Present");
        result = vkQueuePresentKHR(device_->presentQueue(), &presentInfo);
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebufferResized_) {
        framebufferResized_ = false;
        swapChainOutOfDate_ = true;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    return submissionSerial;
}

void FrameSync::onSwapChainRecreated() {
    VkDevice d = device_->logicalDevice();
    for (auto sem : renderFinished_)
        vkDestroySemaphore(d, sem, nullptr);
    renderFinished_.clear();

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished_.resize(swapChain_->imageCount());
    for (uint32_t index = 0; index < renderFinished_.size(); ++index) {
        auto &sem = renderFinished_[index];
        VK_CHECK(vkCreateSemaphore(d, &semInfo, nullptr, &sem));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_SEMAPHORE, sem,
            "Swapchain/Image" + std::to_string(index) + "/RenderFinished");
    }

    swapChainOutOfDate_ = false;
}

// ---- 单次命令辅助 ----

VkCommandBuffer FrameSync::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo,
                             &commandBuffer);
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_COMMAND_BUFFER, commandBuffer,
        "FrameSync/SingleTimeCommandBuffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void FrameSync::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    ++uploadSyncCounters_.singleTimeSubmits;
    vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    ++uploadSyncCounters_.queueWaitIdleCalls;
    vkQueueWaitIdle(device_->graphicsQueue());

    vkFreeCommandBuffers(device_->logicalDevice(), commandPool_, 1,
                         &commandBuffer);
}

// ---- 内部创建 ----

void FrameSync::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = device_->queueFamilies();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    VK_CHECK(vkCreateCommandPool(device_->logicalDevice(), &poolInfo, nullptr,
                                 &commandPool_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_COMMAND_POOL,
                                        commandPool_,
                                        "Frame/CommandPool");
}

void FrameSync::createCommandBuffers() {
    std::vector<VkCommandBuffer> buffers(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VK_CHECK(vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo,
                                      buffers.data()));
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        frames_[i].commandBuffer = buffers[i];
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_COMMAND_BUFFER, frames_[i].commandBuffer,
            "Frame/" + std::to_string(i) + "/CommandBuffer");
    }
}

void FrameSync::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice d = device_->logicalDevice();
    for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
        auto &f = frames_[frame];
        VK_CHECK(
            vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable));
        VK_CHECK(vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_SEMAPHORE, f.imageAvailable,
            "Frame/" + std::to_string(frame) + "/ImageAvailable");
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FENCE, f.inFlight,
            "Frame/" + std::to_string(frame) + "/InFlightFence");
    }

    renderFinished_.resize(swapChain_->imageCount());
    for (uint32_t index = 0; index < renderFinished_.size(); ++index) {
        auto &sem = renderFinished_[index];
        VK_CHECK(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &sem));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_SEMAPHORE, sem,
            "Swapchain/Image" + std::to_string(index) + "/RenderFinished");
    }
}

} // namespace vkr
