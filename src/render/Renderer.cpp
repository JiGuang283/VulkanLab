#include "Renderer.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/SwapChain.h"
#include "core/UploadContext.h"
#include "core/VulkanCheck.h"
#include "render/FrameGpuData.h"
#include "render/EnvironmentGpuResources.h"
#include "render/GuiSystem.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderQueue.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/ShaderVariant.h"
#include "render/Texture.h"
#include "render/pass/DirectionalShadowPass.h"
#include "render/pass/MainForwardPass.h"
#include "render/pass/SkyboxPass.h"
#include "render/pass/ToneMapPass.h"
#include "render/pass/PresentPass.h"
#include "render/pass/BloomPass.h"
#include "diagnostics/TracyProfiler.h"
#include "diagnostics/Profiling.h"

#include <cstring>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vkr {

Renderer::Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
                   DescriptorAllocator &descriptorAllocator,
                   RendererShaderPaths shaderPaths)
    : device_(&device), swapChain_(&swapChain), frameSync_(&frameSync),
      descriptorAllocator_(&descriptorAllocator),
      uniformBufferSize_(sizeof(GlobalFrameUbo)),
      shaderPaths_(std::move(shaderPaths)) {
    createUniformBuffers();
    createGlobalDescriptorSetLayout();
    createGlobalDescriptorSets();
    renderResources_ = std::make_unique<RenderResourceRegistry>(device);
    VkFormatProperties viewportFormatProperties{};
    vkGetPhysicalDeviceFormatProperties(device.physicalDevice(),
                                        swapChain.imageFormat(),
                                        &viewportFormatProperties);
    constexpr VkFormatFeatureFlags kViewportFeatures =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((viewportFormatProperties.optimalTilingFeatures &
         kViewportFeatures) != kViewportFeatures) {
        throw std::runtime_error(
            "swapchain format cannot be used as a sampled viewport target");
    }
    resourceHandles_ = registerDefaultRendererResources(
        *renderResources_, device, swapChain.imageFormat());
    renderResources_->realize(swapChain.extent());
    createLightingDescriptorSetLayout();
    createFallbackEnvironment();
    createLightingGeneration(fallbackEnvironment_);
    createRenderPipeline();
    gpuPassProfiler_ =
        std::make_unique<GpuPassProfiler>(device, pipeline_.passNames());
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());

    if (currentLightingGeneration_) {
        freeLightingGeneration(*currentLightingGeneration_);
        currentLightingGeneration_.reset();
    }
    for (auto &generation : retiredLightingGenerations_)
        freeLightingGeneration(generation);
    retiredLightingGenerations_.clear();
    fallbackEnvironment_.reset();
    uniformBuffers_.clear();
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 lightingDescriptorSetLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 globalDescriptorSetLayout_, nullptr);
}

void Renderer::renderFrame(const FrameSync::FrameContext &frame,
                           const RenderQueue &queue,
                           PipelineCache &pipelineCache,
                           GuiSystem *gui,
                           const ShaderVariant &shaderVariant,
                           const RenderView &view) {
    VKL_PROFILE_ZONE("Renderer::renderFrame");
    std::memcpy(uniformBuffers_[frame.frameIndex]->mappedData(),
                &view.globalUbo, sizeof(view.globalUbo));
    collectRetiredLightingGenerations();

    RenderFrameContext renderFrame{};
    renderFrame.cmd = frame.cmd;
    renderFrame.frameIndex = frame.frameIndex;
    renderFrame.imageIndex = frame.imageIndex;
    renderFrame.viewportExtent = renderResources_->viewportExtent();
    renderFrame.swapchainExtent = swapChain_->extent();
    renderFrame.globalDescriptorSet = globalDescriptorSet(frame.frameIndex);
    renderFrame.globalDescriptorSetLayout = globalDescriptorSetLayout_;
    renderFrame.lightingDescriptorSet =
        currentLightingGeneration_->sets.at(frame.frameIndex);
    renderFrame.lightingDescriptorSetLayout =
        lightingDescriptorSetLayout_;
    renderFrame.pipelineCache = &pipelineCache;
    renderFrame.debugUtils = &device_->debugUtils();
    renderFrame.tracyProfiler = &device_->tracyProfiler();
    renderFrame.gui = gui;
    renderFrame.shaderVariant = &shaderVariant;
    renderFrame.view = &view;
    renderFrame.environmentReady = environmentReady();

    gpuPassProfiler_->collect(frame.frameIndex);
    gpuPassProfiler_->beginFrame(frame.cmd, frame.frameIndex,
                                 frameSync_->lastSubmittedSerial() + 1);
    pipeline_.execute(renderFrame, *renderResources_, queue,
                      gpuPassProfiler_.get());
}

void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());

    pipeline_.releaseSwapChainResources();
    swapChain_->recreate();
    pipeline_.onSwapChainResize(*swapChain_);
}

void Renderer::resizeViewport(VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0 ||
        (extent.width == renderResources_->viewportExtent().width &&
         extent.height == renderResources_->viewportExtent().height)) {
        return;
    }
    const uint32_t maxImageDimension =
        device_->physicalDeviceProperties().limits.maxImageDimension2D;
    extent.width = std::min(extent.width, maxImageDimension);
    extent.height = std::min(extent.height, maxImageDimension);

    pipeline_.releaseViewportResources();
    renderResources_->recreateViewportDependent(extent);
    pipeline_.validateResources(*renderResources_);
    pipeline_.onViewportResize(*renderResources_);
}

VkRenderPass Renderer::renderPass() const {
    return presentPass_ ? presentPass_->renderPass() : VK_NULL_HANDLE;
}

VkExtent2D Renderer::viewportExtent() const {
    return renderResources_->viewportExtent();
}

RendererViewportOutput Renderer::viewportOutput() const {
    RendererViewportOutput output{};
    output.extent = renderResources_->viewportExtent();
    output.format =
        renderResources_->description(resourceHandles_.viewportColor).format;
    output.sampler =
        renderResources_->sampler(resourceHandles_.viewportSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const Image &image =
            renderResources_->image(resourceHandles_.viewportColor, frame);
        output.images[frame] = image.handle();
        output.imageViews[frame] = image.imageView();
    }
    return output;
}

void Renderer::createUniformBuffers() {
    if (uniformBufferSize_ == 0)
        return;
    uniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < uniformBuffers_.size(); ++frame) {
        uniformBuffers_[frame] = std::make_unique<Buffer>(
            *device_, uniformBufferSize_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, "Frame/" + std::to_string(frame) + "/GlobalUniformBuffer");
        uniformBuffers_[frame]->map();
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
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, globalDescriptorSetLayout_,
        "Frame/GlobalDescriptorSetLayout");
}

void Renderer::createGlobalDescriptorSets() {
    if (uniformBuffers_.empty())
        return;

    globalDescriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < globalDescriptorSets_.size(); ++frame) {
        globalDescriptorSets_[frame] = descriptorAllocator_->allocate(
            globalDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}},
            "Frame/" + std::to_string(frame) + "/GlobalDescriptorSet");
    }

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

void Renderer::createLightingDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(
        device_->logicalDevice(), &info, nullptr,
        &lightingDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        lightingDescriptorSetLayout_, "Lighting/DescriptorSetLayout");
}

void Renderer::createFallbackEnvironment() {
    UploadContext upload(*device_, nullptr, 64 * 1024,
                         "EnvironmentFallback");
    const bool useFloat = device_->environmentIblSupported();
    const VkFormat cubeFormat =
        useFloat ? VK_FORMAT_R16G16B16A16_SFLOAT
                 : VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat lutFormat =
        useFloat ? VK_FORMAT_R16G16_SFLOAT
                 : VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t cubePixelBytes = useFloat ? 8u : 4u;
    const uint32_t lutPixelBytes = useFloat ? 4u : 4u;
    std::vector<uint8_t> cubeBytes(cubePixelBytes * 6, 0);
    std::array<TextureSubresourceInfo, 6> cubeSubresources{};
    for (uint32_t face = 0; face < cubeSubresources.size(); ++face) {
        cubeSubresources[face] = {
            static_cast<VkDeviceSize>(face * cubePixelBytes),
            cubePixelBytes, 1, 1, 0, face};
    }
    TextureCreateInfo cubeInfo{};
    cubeInfo.pixels = cubeBytes.data();
    cubeInfo.dataSize = cubeBytes.size();
    cubeInfo.width = 1;
    cubeInfo.height = 1;
    cubeInfo.generateMipmaps = false;
    cubeInfo.format = cubeFormat;
    cubeInfo.wrapU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cubeInfo.wrapV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cubeInfo.wrapW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cubeInfo.subresources = cubeSubresources.data();
    cubeInfo.subresourceCount =
        static_cast<uint32_t>(cubeSubresources.size());
    cubeInfo.arrayLayers = 6;
    cubeInfo.imageFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    cubeInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeInfo.debugName = "Environment/Fallback/Cube";
    auto cube =
        std::make_shared<Texture>(*device_, upload, cubeInfo);

    std::vector<uint8_t> lutBytes(lutPixelBytes, 0);
    TextureSubresourceInfo lutSubresource{
        0, lutPixelBytes, 1, 1, 0, 0};
    TextureCreateInfo lutInfo{};
    lutInfo.pixels = lutBytes.data();
    lutInfo.dataSize = lutBytes.size();
    lutInfo.width = 1;
    lutInfo.height = 1;
    lutInfo.generateMipmaps = false;
    lutInfo.format = lutFormat;
    lutInfo.wrapU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lutInfo.wrapV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lutInfo.subresources = &lutSubresource;
    lutInfo.subresourceCount = 1;
    lutInfo.debugName = "Environment/Fallback/BrdfLut";
    auto lut = std::make_shared<Texture>(*device_, upload, lutInfo);
    upload.finish();

    fallbackEnvironment_ =
        std::make_shared<EnvironmentGpuResources>();
    fallbackEnvironment_->radiance = cube;
    fallbackEnvironment_->irradiance = cube;
    fallbackEnvironment_->prefilteredSpecular = std::move(cube);
    fallbackEnvironment_->brdfLut = std::move(lut);
}

void Renderer::createLightingGeneration(
    std::shared_ptr<EnvironmentGpuResources> environment) {
    if (!environment)
        environment = fallbackEnvironment_;
    auto generation =
        std::make_unique<LightingDescriptorGeneration>();
    generation->environment = std::move(environment);
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        VkDescriptorSet set = descriptorAllocator_->allocate(
            lightingDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}},
            "Lighting/DescriptorSet/Frame" +
                std::to_string(frameIndex));
        generation->sets[frameIndex] = set;
        std::array<VkDescriptorImageInfo, 5> images{};
        images[0].sampler =
            renderResources_->sampler(resourceHandles_.shadowSampler);
        images[0].imageView =
            renderResources_
                ->image(resourceHandles_.directionalShadowDepth,
                        frameIndex)
                .imageView();
        images[0].imageLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        const auto fillEnvironment =
            [&](uint32_t binding, const std::shared_ptr<Texture> &texture) {
                images[binding].sampler = texture->sampler();
                images[binding].imageView = texture->imageView();
                images[binding].imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            };
        fillEnvironment(1, generation->environment->irradiance);
        fillEnvironment(2,
                        generation->environment->prefilteredSpecular);
        fillEnvironment(3, generation->environment->brdfLut);
        fillEnvironment(4, generation->environment->radiance);

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = &images[binding];
        }
        vkUpdateDescriptorSets(
            device_->logicalDevice(), static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);
    }
    if (currentLightingGeneration_) {
        currentLightingGeneration_->retireAfterSerial =
            frameSync_->lastSubmittedSerial();
        retiredLightingGenerations_.push_back(
            std::move(*currentLightingGeneration_));
    }
    currentLightingGeneration_ = std::move(generation);
}

void Renderer::freeLightingGeneration(
    LightingDescriptorGeneration &generation) {
    for (VkDescriptorSet &set : generation.sets) {
        descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
    generation.environment.reset();
}

void Renderer::collectRetiredLightingGenerations() {
    const uint64_t completed =
        frameSync_->completedSubmissionSerial();
    while (!retiredLightingGenerations_.empty() &&
           retiredLightingGenerations_.front().retireAfterSerial <=
               completed) {
        freeLightingGeneration(retiredLightingGenerations_.front());
        retiredLightingGenerations_.pop_front();
    }
}

void Renderer::publishEnvironment(
    std::shared_ptr<EnvironmentGpuResources> environment) {
    createLightingGeneration(std::move(environment));
}

void Renderer::clearEnvironment() {
    createLightingGeneration(fallbackEnvironment_);
}

bool Renderer::environmentReady() const {
    return currentLightingGeneration_ &&
           currentLightingGeneration_->environment &&
           !currentLightingGeneration_->environment->environmentId.empty();
}

std::string Renderer::currentEnvironmentId() const {
    return environmentReady()
               ? currentLightingGeneration_->environment->environmentId
               : std::string{};
}

float Renderer::currentEnvironmentMaxSpecularLod() const {
    return environmentReady()
               ? currentLightingGeneration_->environment->maxSpecularLod
               : 0.0f;
}

bool Renderer::bloomSupported() const {
    return device_->computeBloomSupport().available;
}

const std::string &Renderer::bloomUnsupportedReason() const {
    return device_->computeBloomSupport().reason;
}

void Renderer::createRenderPipeline() {
    pipeline_.addPass(std::make_unique<DirectionalShadowPass>(
        *device_, *renderResources_, resourceHandles_.directionalShadowDepth,
        globalDescriptorSetLayout_,
        shaderPaths_.shadowVert, shaderPaths_.shadowMaskFrag));

    pipeline_.addPass(std::make_unique<SkyboxPass>(
        *device_, *renderResources_, resourceHandles_,
        globalDescriptorSetLayout_, lightingDescriptorSetLayout_,
        shaderPaths_.fullscreenVert, shaderPaths_.skyboxFrag));

    auto mainPass = std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_,
        lightingDescriptorSetLayout_);
    mainForwardPass_ = mainPass.get();
    pipeline_.addPass(std::move(mainPass));

    if (device_->computeBloomSupport().available) {
        pipeline_.addPass(std::make_unique<BloomPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, shaderPaths_.bloomDownsampleComp,
            shaderPaths_.bloomUpsampleComp));
    }

    auto toneMapPass = std::make_unique<ToneMapPass>(
        *device_, *renderResources_, resourceHandles_.hdrColor,
        resourceHandles_.hdrSampler, resourceHandles_.bloomLevels.front(),
        resourceHandles_.bloomSampler, resourceHandles_.viewportColor,
        *descriptorAllocator_,
        shaderPaths_.fullscreenVert, shaderPaths_.toneMapFrag);
    toneMapPass_ = toneMapPass.get();
    pipeline_.addPass(std::move(toneMapPass));

    auto presentPass = std::make_unique<PresentPass>(
        *device_, *swapChain_, *renderResources_,
        resourceHandles_.viewportColor, resourceHandles_.viewportSampler,
        *descriptorAllocator_, shaderPaths_.fullscreenVert,
        shaderPaths_.presentFrag);
    presentPass_ = presentPass.get();
    pipeline_.addPass(std::move(presentPass));
    pipeline_.validateResources(*renderResources_);
}

const GpuPassTimings &Renderer::gpuPassTimings() const {
    return gpuPassProfiler_->latest();
}

VkDescriptorSet Renderer::globalDescriptorSet(uint32_t frameIndex) const {
    return globalDescriptorSets_[frameIndex];
}

} // namespace vkr
