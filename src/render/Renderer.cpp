#include "Renderer.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "core/VulkanCheck.h"
#include "render/GuiSystem.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderQueue.h"
#include "render/ShaderVariant.h"
#include "render/pass/MainForwardPass.h"

#include <memory>
#include <utility>

namespace vkr {

Renderer::Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
                   DescriptorAllocator &descriptorAllocator,
                   VkDeviceSize uniformBufferSize)
    : device_(&device), swapChain_(&swapChain), frameSync_(&frameSync),
      descriptorAllocator_(&descriptorAllocator),
      uniformBufferSize_(uniformBufferSize) {
    createUniformBuffers();
    createGlobalDescriptorSetLayout();
    createGlobalDescriptorSets();
    createRenderPipeline();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());

    uniformBuffers_.clear();
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 globalDescriptorSetLayout_, nullptr);
}

void Renderer::renderFrame(const FrameSync::FrameContext &frame,
                           const RenderQueue &queue,
                           PipelineCache &pipelineCache,
                           GuiSystem &gui,
                           const ShaderVariant &shaderVariant) {
    RenderFrameContext renderFrame{};
    renderFrame.cmd = frame.cmd;
    renderFrame.frameIndex = frame.frameIndex;
    renderFrame.imageIndex = frame.imageIndex;
    renderFrame.extent = swapChain_->extent();
    renderFrame.globalDescriptorSet = globalDescriptorSet(frame.frameIndex);
    renderFrame.globalDescriptorSetLayout = globalDescriptorSetLayout_;
    renderFrame.pipelineCache = &pipelineCache;
    renderFrame.gui = &gui;
    renderFrame.shaderVariant = &shaderVariant;

    pipeline_.execute(renderFrame, queue);
}

void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());

    swapChain_->recreate();
    pipeline_.onResize(*swapChain_);
}

VkRenderPass Renderer::renderPass() const {
    return mainForwardPass_ ? mainForwardPass_->renderPass() : VK_NULL_HANDLE;
}

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
    uboLayoutBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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
        set = descriptorAllocator_->allocate(
            globalDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}});

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

void Renderer::createRenderPipeline() {
    auto mainPass =
        std::make_unique<MainForwardPass>(*device_, *swapChain_, *frameSync_);
    mainForwardPass_ = mainPass.get();
    pipeline_.addPass(std::move(mainPass));
}

void *Renderer::mappedUniformBuffer(uint32_t frameIndex) const {
    return uniformBuffers_[frameIndex]->mappedData();
}

VkDescriptorSet Renderer::globalDescriptorSet(uint32_t frameIndex) const {
    return globalDescriptorSets_[frameIndex];
}

} // namespace vkr
