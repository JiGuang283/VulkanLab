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
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"
#include "render/ShaderVariant.h"
#include "render/Texture.h"
#include "render/pass/DirectionalShadowPass.h"
#include "render/pass/VisibilityDepthPass.h"
#include "render/pass/HiZBuildPass.h"
#include "render/pass/OcclusionCullPass.h"
#include "render/pass/MainForwardPass.h"
#include "render/pass/SkyBackgroundPass.h"
#include "render/pass/ToneMapPass.h"
#include "render/pass/PresentPass.h"
#include "render/pass/BloomPass.h"
#include "render/pass/AtmosphereLutPass.h"
#include "diagnostics/TracyProfiler.h"
#include "diagnostics/Profiling.h"

#include <cstring>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kInitialSceneLightCapacity = 16;

uint32_t nextSceneLightCapacity(uint32_t required) {
    uint32_t capacity = kInitialSceneLightCapacity;
    while (capacity < required && capacity < kMaxSceneLights)
        capacity *= 2;
    return std::min(capacity, kMaxSceneLights);
}

} // namespace

Renderer::Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
                   DescriptorAllocator &descriptorAllocator,
                   RendererShaderPaths shaderPaths)
    : device_(&device), swapChain_(&swapChain), frameSync_(&frameSync),
      descriptorAllocator_(&descriptorAllocator),
      uniformBufferSize_(sizeof(GlobalFrameUbo)),
      shaderPaths_(std::move(shaderPaths)) {
    createUniformBuffers();
    createSceneLightBuffers();
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
    initializeAtmosphereImages();
    createLightingDescriptorSetLayout();
    createFallbackEnvironment();
    createLightingGeneration(fallbackEnvironment_);
    createAtmosphereUniformBuffers();
    createAtmosphereDescriptorSetLayout();
    createAtmosphereDescriptorSets();
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
    for (VkDescriptorSet &set : atmosphereDescriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
    atmosphereUniformBuffers_.clear();
    uniformBuffers_.clear();
    for (auto &storage : sceneLightBuffers_)
        storage.buffer.reset();
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 lightingDescriptorSetLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 atmosphereDescriptorSetLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 globalDescriptorSetLayout_, nullptr);
}

void Renderer::renderFrame(const FrameSync::FrameContext &frame,
                           VisibilityFrame &visibility,
                           PipelineCache &pipelineCache,
                           GuiSystem *gui,
                           const ShaderVariant &shaderVariant,
                           const RenderView &view) {
    VKL_PROFILE_ZONE("Renderer::renderFrame");
    atmosphereStatus_.supported = device_->atmosphereSupport().available;
    atmosphereStatus_.unavailableReason = device_->atmosphereSupport().reason;
    atmosphereStatus_.componentPresent =
        view.atmosphere.componentPresent;
    atmosphereStatus_.active = view.atmosphere.active;
    atmosphereStatus_.componentEntity = view.atmosphere.componentEntity;
    atmosphereStatus_.sunEntity = view.atmosphere.sunEntity;
    atmosphereStatus_.sunBufferIndex = view.atmosphere.sunBufferIndex;
    atmosphereStatus_.cameraAltitudeKm = view.atmosphere.cameraAltitudeKm;
    std::memcpy(uniformBuffers_[frame.frameIndex]->mappedData(),
                 &view.globalUbo, sizeof(view.globalUbo));
    std::memcpy(atmosphereUniformBuffers_[frame.frameIndex]->mappedData(),
                &view.atmosphereGpuParams,
                sizeof(view.atmosphereGpuParams));
    ensureSceneLightCapacity(
        frame.frameIndex,
        static_cast<uint32_t>(view.sceneLights.size()));
    FrameSceneLightStorage &lightStorage =
        sceneLightBuffers_.at(frame.frameIndex);
    if (!view.sceneLights.empty()) {
        std::memcpy(lightStorage.buffer->mappedData(),
                    view.sceneLights.data(),
                    view.sceneLights.size() * sizeof(GpuLight));
    }
    activeSceneLightCount_ =
        static_cast<uint32_t>(view.sceneLights.size());
    collectRetiredLightingGenerations();

    if (occlusionCullPass_) {
        occlusionCullPass_->prepareFrame(
            frame.frameIndex, frameSync_->lastSubmittedSerial() + 1,
            visibility, view);
        lastOcclusionFrameIndex_ = frame.frameIndex;
    }

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
    renderFrame.atmosphereDescriptorSet =
        atmosphereDescriptorSets_.at(frame.frameIndex);
    renderFrame.atmosphereDescriptorSetLayout =
        atmosphereDescriptorSetLayout_;
    renderFrame.pipelineCache = &pipelineCache;
    renderFrame.debugUtils = &device_->debugUtils();
    renderFrame.tracyProfiler = &device_->tracyProfiler();
    renderFrame.gui = gui;
    renderFrame.shaderVariant = &shaderVariant;
    renderFrame.view = &view;
    renderFrame.environmentReady = environmentReady();
    renderFrame.atmosphereReady =
        atmosphereLutPass_ &&
        atmosphereLutPass_->readyFor(view.atmosphere.staticLutKey);
    renderFrame.occlusionActive =
        occlusionCullPass_ && occlusionCullPass_->active(frame.frameIndex);
    renderFrame.occlusionIndirectBuffer =
        occlusionCullPass_
            ? occlusionCullPass_->indirectBuffer(frame.frameIndex)
            : VK_NULL_HANDLE;

    gpuPassProfiler_->collect(frame.frameIndex);
    gpuPassProfiler_->beginFrame(frame.cmd, frame.frameIndex,
                                 frameSync_->lastSubmittedSerial() + 1);
    pipeline_.execute(renderFrame, *renderResources_, visibility,
                      gpuPassProfiler_.get());
    if (atmosphereLutPass_)
        atmosphereStatus_ = atmosphereLutPass_->status();
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

void Renderer::createSceneLightBuffers() {
    for (uint32_t frame = 0; frame < sceneLightBuffers_.size(); ++frame) {
        FrameSceneLightStorage &storage = sceneLightBuffers_[frame];
        storage.capacity = kInitialSceneLightCapacity;
        storage.buffer = std::make_unique<Buffer>(
            *device_, sizeof(GpuLight) * storage.capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, "Frame/" + std::to_string(frame) + "/SceneLightBuffer");
        storage.buffer->map();
    }
}

void Renderer::ensureSceneLightCapacity(uint32_t frameIndex,
                                        uint32_t requiredLights) {
    if (requiredLights > kMaxSceneLights)
        throw std::runtime_error("RenderView exceeded the scene light limit");

    FrameSceneLightStorage &storage = sceneLightBuffers_.at(frameIndex);
    if (requiredLights <= storage.capacity)
        return;

    const uint32_t capacity = nextSceneLightCapacity(requiredLights);
    storage.buffer = std::make_unique<Buffer>(
        *device_, sizeof(GpuLight) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        0, "Frame/" + std::to_string(frameIndex) +
               "/SceneLightBuffer/Capacity" + std::to_string(capacity));
    storage.buffer->map();
    storage.capacity = capacity;
    updateSceneLightDescriptor(frameIndex);
}

void Renderer::createGlobalDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

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
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
            "Frame/" + std::to_string(frame) + "/GlobalDescriptorSet");
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i]->handle();
        bufferInfo.offset = 0;
        bufferInfo.range = uniformBufferSize_;

        VkDescriptorBufferInfo lightBufferInfo{};
        lightBufferInfo.buffer = sceneLightBuffers_[i].buffer->handle();
        lightBufferInfo.offset = 0;
        lightBufferInfo.range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = globalDescriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = globalDescriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &lightBufferInfo;

        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void Renderer::updateSceneLightDescriptor(uint32_t frameIndex) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = sceneLightBuffers_.at(frameIndex).buffer->handle();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = globalDescriptorSets_.at(frameIndex);
    write.dstBinding = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0, nullptr);
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

void Renderer::createAtmosphereUniformBuffers() {
    atmosphereUniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < atmosphereUniformBuffers_.size();
         ++frame) {
        atmosphereUniformBuffers_[frame] = std::make_unique<Buffer>(
            *device_, sizeof(AtmosphereGpuParams),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, "Frame/" + std::to_string(frame) +
                   "/AtmosphereUniformBuffer");
        atmosphereUniformBuffers_[frame]->map();
    }
}

void Renderer::createAtmosphereDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t binding = 1; binding < bindings.size(); ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags =
            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(
        device_->logicalDevice(), &info, nullptr,
        &atmosphereDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        atmosphereDescriptorSetLayout_, "Atmosphere/DescriptorSetLayout");
}

void Renderer::createAtmosphereDescriptorSets() {
    const std::array<RenderImageHandle, 4> images = {
        resourceHandles_.atmosphereTransmittance,
        resourceHandles_.atmosphereMultipleScattering,
        resourceHandles_.atmosphereSkyView,
        resourceHandles_.atmosphereAerialPerspective};
    const VkSampler sampler =
        renderResources_->sampler(resourceHandles_.atmosphereSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        VkDescriptorSet &set = atmosphereDescriptorSets_[frame];
        set = descriptorAllocator_->allocate(
            atmosphereDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}},
            "Frame/" + std::to_string(frame) +
                "/AtmosphereDescriptorSet");
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = atmosphereUniformBuffers_[frame]->handle();
        bufferInfo.range = sizeof(AtmosphereGpuParams);
        std::array<VkDescriptorImageInfo, 4> imageInfos{};
        for (uint32_t index = 0; index < images.size(); ++index) {
            imageInfos[index].sampler = sampler;
            imageInfos[index].imageView =
                renderResources_->image(images[index], frame).imageView();
            imageInfos[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        std::array<VkWriteDescriptorSet, 5> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;
        for (uint32_t index = 0; index < imageInfos.size(); ++index) {
            writes[index + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index + 1].dstSet = set;
            writes[index + 1].dstBinding = index + 1;
            writes[index + 1].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index + 1].descriptorCount = 1;
            writes[index + 1].pImageInfo = &imageInfos[index];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void Renderer::initializeAtmosphereImages() {
    UploadContext upload(*device_, nullptr, 64 * 1024,
                         "AtmosphereFallbacks");
    VkCommandBuffer cmd = upload.commandBuffer();
    const std::array<RenderImageHandle, 4> handles = {
        resourceHandles_.atmosphereTransmittance,
        resourceHandles_.atmosphereMultipleScattering,
        resourceHandles_.atmosphereSkyView,
        resourceHandles_.atmosphereAerialPerspective};
    for (RenderImageHandle handle : handles) {
        const RenderImageDesc &desc = renderResources_->description(handle);
        const uint32_t count =
            desc.multiplicity == RenderResourceMultiplicity::PerFrame
                ? MAX_FRAMES_IN_FLIGHT
                : 1u;
        for (uint32_t frame = 0; frame < count; ++frame) {
            const Image &image = renderResources_->image(handle, frame);
            VkImageMemoryBarrier toClear{};
            toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.image = image.handle();
            toClear.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toClear.subresourceRange.levelCount = 1;
            toClear.subresourceRange.layerCount = desc.arrayLayers;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toClear);
            VkClearColorValue clear{};
            if (handle == resourceHandles_.atmosphereTransmittance)
                clear.float32[0] = clear.float32[1] =
                    clear.float32[2] = clear.float32[3] = 1.0f;
            else
                clear.float32[3] = 1.0f;
            vkCmdClearColorImage(cmd, image.handle(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear, 1, &toClear.subresourceRange);
            VkImageMemoryBarrier toSample = toClear;
            toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toSample);
        }
    }
    upload.finish();
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

bool Renderer::atmosphereSupported() const {
    return device_->atmosphereSupport().available;
}

const std::string &Renderer::atmosphereUnsupportedReason() const {
    return device_->atmosphereSupport().reason;
}

AtmosphereRuntimeStatus Renderer::atmosphereStatus() const {
    return atmosphereStatus_;
}

SceneLightBufferStatus Renderer::sceneLightBufferStatus() const {
    SceneLightBufferStatus status{};
    status.activeLights = activeSceneLightCount_;
    for (uint32_t frame = 0; frame < sceneLightBuffers_.size(); ++frame) {
        status.frameCapacities[frame] = sceneLightBuffers_[frame].capacity;
        status.allocatedBytes +=
            static_cast<uint64_t>(sceneLightBuffers_[frame].capacity) *
            sizeof(GpuLight);
    }
    return status;
}

OcclusionCullingStatus Renderer::occlusionCullingStatus() const {
    OcclusionCullingStatus status{};
    status.supported = device_->occlusionCullingSupport().available;
    status.unavailableReason = device_->occlusionCullingSupport().reason;
    if (!occlusionCullPass_)
        return status;

    status.active = occlusionCullPass_->active(lastOcclusionFrameIndex_);
    if (resourceHandles_.visibilityHiZ.valid()) {
        status.hiZMipLevels =
            renderResources_->mipLevelCount(resourceHandles_.visibilityHiZ);
    }
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        status.indirectCapacities[frame] =
            occlusionCullPass_->capacity(frame);
    }
    status.allocatedBytes = occlusionCullPass_->allocatedBytes();
    return status;
}

void Renderer::createRenderPipeline() {
    if (device_->atmosphereSupport().available) {
        auto atmospherePass = std::make_unique<AtmosphereLutPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, atmosphereDescriptorSetLayout_,
            shaderPaths_.atmosphereTransmittanceComp,
            shaderPaths_.atmosphereMultipleScatteringComp,
            shaderPaths_.atmosphereSkyViewComp,
            shaderPaths_.atmosphereAerialPerspectiveComp);
        atmosphereLutPass_ = atmospherePass.get();
        pipeline_.addPass(std::move(atmospherePass));
    }
    pipeline_.addPass(std::make_unique<DirectionalShadowPass>(
        *device_, *renderResources_, resourceHandles_.directionalShadowDepth,
        globalDescriptorSetLayout_,
        shaderPaths_.shadowVert, shaderPaths_.shadowMaskFrag));

    if (device_->occlusionCullingSupport().available) {
        pipeline_.addPass(std::make_unique<VisibilityDepthPass>(
            *device_, *renderResources_, resourceHandles_.visibilityDepth,
            globalDescriptorSetLayout_, shaderPaths_.visibilityDepthVert,
            shaderPaths_.visibilityDepthMaskFrag));
        pipeline_.addPass(std::make_unique<HiZBuildPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, shaderPaths_.visibilityHiZInitComp,
            shaderPaths_.visibilityHiZReduceComp));
        auto occlusionPass = std::make_unique<OcclusionCullPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, shaderPaths_.visibilityOcclusionComp);
        occlusionCullPass_ = occlusionPass.get();
        pipeline_.addPass(std::move(occlusionPass));
    }

    pipeline_.addPass(std::make_unique<SkyBackgroundPass>(
        *device_, *renderResources_, resourceHandles_,
        globalDescriptorSetLayout_, lightingDescriptorSetLayout_,
        atmosphereDescriptorSetLayout_, shaderPaths_.fullscreenVert,
        shaderPaths_.skyboxFrag, shaderPaths_.atmosphereSkyFrag));

    auto mainPass = std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_,
        lightingDescriptorSetLayout_, atmosphereDescriptorSetLayout_);
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
