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
#include "render/TemporalAA.h"
#include "render/Visibility.h"
#include "render/ShaderVariant.h"
#include "render/Texture.h"
#include "render/pass/DirectionalShadowPass.h"
#include "render/pass/SurfacePrepass.h"
#include "render/pass/HiZBuildPass.h"
#include "render/pass/HdrCompositePass.h"
#include "render/pass/OcclusionCullPass.h"
#include "render/pass/ScreenSpacePyramidPass.h"
#include "render/pass/SsaoPass.h"
#include "render/pass/SsrPass.h"
#include "render/pass/SsgiPass.h"
#include "render/pass/CacaoNormalAdapterPass.h"
#include "render/pass/CacaoPass.h"
#include "render/pass/GtaoPass.h"
#include "render/pass/MainForwardPass.h"
#include "render/pass/SkyBackgroundPass.h"
#include "render/pass/ToneMapPass.h"
#include "render/pass/TaaPass.h"
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
    createScreenSpaceUniformBuffers();
    createScreenSpaceDescriptorSetLayout();
    createScreenSpaceFallback();
    createScreenSpaceDescriptorSets();
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
    for (VkDescriptorSet &set : screenSpaceDescriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
    screenSpaceWhiteFallback_.reset();
    screenSpaceUniformBuffers_.clear();
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
                                 screenSpaceDescriptorSetLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                 globalDescriptorSetLayout_, nullptr);
}

void Renderer::renderFrame(const FrameSync::FrameContext &frame,
                           const VisibilityFrame &visibility,
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

    const ScreenSpaceEffectsSupport &screenSupport =
        device_->screenSpaceEffectsSupport();
    const bool ssaoDebugRequested =
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsaoRaw ||
        view.settings.screenSpaceDebugView ==
            ScreenSpaceDebugView::SsaoFiltered;
    const bool cacaoDebugRequested =
        view.settings.screenSpaceDebugView ==
        ScreenSpaceDebugView::CacaoOutput;
    const bool gtaoDebugRequested =
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::GtaoRaw ||
        view.settings.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoTemporal ||
        view.settings.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoFiltered ||
        view.settings.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoRejection ||
        view.settings.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoHistoryWeight;
    const bool ssaoShadingActive =
        screenSupport.ssaoAvailable && shaderVariant.supportsScreenSpace &&
        view.settings.ambientOcclusionMode == AmbientOcclusionMode::Ssao;
    const bool ssaoPassRequired = ssaoShadingActive || ssaoDebugRequested;
    const bool cacaoReady = cacaoPass_ && cacaoPass_->status().initialized;
    const bool cacaoShadingActive =
        cacaoReady && shaderVariant.supportsScreenSpace &&
        view.settings.ambientOcclusionMode == AmbientOcclusionMode::Cacao;
    const bool cacaoPassRequired =
        cacaoReady && (cacaoShadingActive || cacaoDebugRequested);
    const bool gtaoShadingActive =
        screenSupport.gtaoAvailable && shaderVariant.supportsScreenSpace &&
        view.settings.ambientOcclusionMode == AmbientOcclusionMode::Gtao;
    const bool gtaoPassRequired =
        screenSupport.gtaoAvailable &&
        (gtaoShadingActive || gtaoDebugRequested);
    const bool taaDebugRequested =
        isTaaDebugView(view.settings.screenSpaceDebugView);
    const bool taaShadingActive =
        screenSupport.taaAvailable &&
        view.settings.temporalAntiAliasingMode ==
            TemporalAntiAliasingMode::Taa;
    const bool taaPassRequired =
        screenSupport.taaAvailable &&
        (taaShadingActive || taaDebugRequested);
    const bool ssrDebugRequested =
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsrRaw ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsrTemporal ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsrFiltered ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsrConfidence ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsrRejection;
    const bool ssrShadingActive =
        screenSupport.ssrAvailable && shaderVariant.supportsScreenSpace &&
        view.settings.reflectionMode == ReflectionMode::Ssr;
    const bool ssrPassRequired = screenSupport.ssrAvailable &&
        (ssrShadingActive || ssrDebugRequested);
    const bool ssgiDebugRequested =
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiRaw ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiTemporal ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiFiltered ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiConfidence ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiVariance ||
        view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SsgiRejection;
    const bool ssgiShadingActive =
        screenSupport.ssgiAvailable && shaderVariant.supportsScreenSpace &&
        view.settings.globalIlluminationMode == GlobalIlluminationMode::Ssgi;
    const bool ssgiPassRequired = screenSupport.ssgiAvailable &&
        (ssgiShadingActive || ssgiDebugRequested);
    const AmbientOcclusionMode activeAoMode =
        gtaoShadingActive
            ? AmbientOcclusionMode::Gtao
            : (cacaoShadingActive
                   ? AmbientOcclusionMode::Cacao
                   : (ssaoShadingActive ? AmbientOcclusionMode::Ssao
                                        : AmbientOcclusionMode::Off));
    ScreenSpaceLightingUbo screenUbo{};
    const VkExtent2D screenExtent = renderResources_->viewportExtent();
    screenUbo.viewportSizeInvSize =
        glm::vec4(static_cast<float>(screenExtent.width),
                  static_cast<float>(screenExtent.height),
                  1.0f / static_cast<float>(screenExtent.width),
                  1.0f / static_cast<float>(screenExtent.height));
    screenUbo.modes =
        glm::uvec4(static_cast<uint32_t>(view.settings.ambientOcclusionMode),
                   activeAoMode != AmbientOcclusionMode::Off ? 1u : 0u,
                   0u, 0u);
    std::memcpy(screenSpaceUniformBuffers_[frame.frameIndex]->mappedData(),
                &screenUbo, sizeof(screenUbo));
    updateScreenSpaceDescriptor(frame.frameIndex, activeAoMode);

    if (occlusionCullPass_) {
        lastOcclusionRequested_ = visibility.cpuStats.occlusionCandidates;
        occlusionCullPass_->prepareFrame(
            frame.frameIndex, frameSync_->lastSubmittedSerial() + 1,
            visibility, view);
        lastOcclusionFrameIndex_ = frame.frameIndex;
    }

    RenderFrameContext renderFrame{};
    renderFrame.cmd = frame.cmd;
    renderFrame.frameIndex = frame.frameIndex;
    renderFrame.imageIndex = frame.imageIndex;
    renderFrame.submissionSerial =
        frameSync_->lastSubmittedSerial() + 1u;
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
    renderFrame.screenSpaceDescriptorSet =
        screenSpaceDescriptorSets_.at(frame.frameIndex);
    renderFrame.screenSpaceDescriptorSetLayout =
        screenSpaceDescriptorSetLayout_;
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
    renderFrame.features.surfaceDataRequired =
        device_->surfaceDataSupport().available &&
        (view.settings.culling.occlusionEnabled ||
         view.settings.surfaceDebugView != SurfaceDebugView::None ||
         view.settings.screenSpaceDebugView ==
             ScreenSpaceDebugView::NearestDepth ||
          (screenSupport.ssaoAvailable && ssaoPassRequired) ||
          (device_->cacaoSupport().available && cacaoPassRequired) ||
          gtaoPassRequired ||
          ssrPassRequired ||
          ssgiPassRequired ||
          taaPassRequired);
    renderFrame.features.hiZRequired =
        device_->occlusionCullingSupport().available &&
        view.settings.culling.occlusionEnabled;
    renderFrame.features.occlusionRequired =
        renderFrame.features.hiZRequired;
    renderFrame.features.screenDepthPyramidRequired =
        screenSupport.depthPyramidAvailable &&
        (view.settings.screenSpaceDebugView ==
             ScreenSpaceDebugView::NearestDepth ||
         gtaoPassRequired);
    renderFrame.features.screenDepthPyramidRequired =
        renderFrame.features.screenDepthPyramidRequired || ssrPassRequired ||
        ssgiPassRequired;
    renderFrame.features.sceneColorPyramidRequired =
        screenSupport.colorPyramidAvailable &&
        (view.settings.screenSpaceDebugView == ScreenSpaceDebugView::SceneColor ||
         ssrPassRequired || ssgiPassRequired);
    renderFrame.features.ssaoRequired =
        screenSupport.ssaoAvailable && ssaoPassRequired;
    renderFrame.features.cacaoRequired = cacaoPassRequired;
    renderFrame.features.gtaoRequired = gtaoPassRequired;
    renderFrame.features.taaRequired = taaPassRequired;
    renderFrame.features.taaActive = taaShadingActive;
    renderFrame.features.ssrRequired = ssrPassRequired;
    renderFrame.features.ssrActive = ssrShadingActive;
    renderFrame.features.ssgiRequired = ssgiPassRequired;
    renderFrame.features.ssgiActive = ssgiShadingActive;
    lastSurfaceDataActive_ = renderFrame.features.surfaceDataRequired;

    screenSpaceStatus_.requestedMode = view.settings.ambientOcclusionMode;
    screenSpaceStatus_.activeMode = activeAoMode;
    screenSpaceStatus_.requestedGiMode =
        view.settings.globalIlluminationMode;
    screenSpaceStatus_.activeGiMode =
        ssgiShadingActive ? GlobalIlluminationMode::Ssgi
                          : GlobalIlluminationMode::AmbientOrIbl;
    renderFrame.visibilityDrawStream =
        occlusionCullPass_
            ? &occlusionCullPass_->drawStream(frame.frameIndex)
            : nullptr;

    gpuPassProfiler_->collect(frame.frameIndex);
    gpuPassProfiler_->beginFrame(frame.cmd, frame.frameIndex,
                                 frameSync_->lastSubmittedSerial() + 1);
    pipeline_.execute(renderFrame, *renderResources_, visibility,
                      gpuPassProfiler_.get());
    if (taaPass_) {
        const TaaPassStatus &taa = taaPass_->status();
        screenSpaceStatus_.taaActive = taa.active;
        screenSpaceStatus_.taaHistoryValid = taa.historyValid;
        screenSpaceStatus_.taaHistoryGeneration = taa.historyGeneration;
        screenSpaceStatus_.taaLastFrameSerial = taa.lastFrameSerial;
        screenSpaceStatus_.taaJitterPixels = taa.jitterPixels;
        screenSpaceStatus_.taaLastResetReason = taa.lastResetReason;
    }
    if (gtaoPass_) {
        const GtaoPassStatus &gtao = gtaoPass_->status();
        screenSpaceStatus_.gtaoActive =
            activeAoMode == AmbientOcclusionMode::Gtao;
        screenSpaceStatus_.gtaoHistoryValid = gtao.historyValid;
        screenSpaceStatus_.gtaoExtent = gtao.extent;
        screenSpaceStatus_.gtaoHistoryGeneration = gtao.historyGeneration;
        screenSpaceStatus_.gtaoLastFrameSerial = gtao.lastFrameSerial;
        screenSpaceStatus_.gtaoLastResetReason = gtao.lastResetReason;
    }
    if (ssrPass_) {
        const SsrPassStatus &ssr = ssrPass_->status();
        screenSpaceStatus_.ssrActive = ssr.active;
        screenSpaceStatus_.ssrHistoryValid = ssr.historyValid;
        screenSpaceStatus_.ssrExtent = ssr.extent;
        screenSpaceStatus_.ssrHistoryGeneration = ssr.historyGeneration;
        screenSpaceStatus_.ssrLastFrameSerial = ssr.lastFrameSerial;
        screenSpaceStatus_.ssrLastResetReason = ssr.lastResetReason;
    }
    if (ssgiPass_) {
        const SsgiPassStatus &ssgi = ssgiPass_->status();
        screenSpaceStatus_.ssgiActive = ssgi.active;
        screenSpaceStatus_.ssgiHistoryValid = ssgi.historyValid;
        screenSpaceStatus_.ssgiExtent = ssgi.extent;
        screenSpaceStatus_.ssgiHistoryGeneration = ssgi.historyGeneration;
        screenSpaceStatus_.ssgiLastFrameSerial = ssgi.lastFrameSerial;
        screenSpaceStatus_.ssgiLastResetReason = ssgi.lastResetReason;
    }
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

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
        updateScreenSpaceDescriptor(frame, AmbientOcclusionMode::Off);
    pipeline_.releaseViewportResources();
    renderResources_->recreateViewportDependent(extent);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
        updateScreenSpaceDescriptor(frame, AmbientOcclusionMode::Off);
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
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
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

void Renderer::createScreenSpaceUniformBuffers() {
    screenSpaceUniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        screenSpaceUniformBuffers_[frame] = std::make_unique<Buffer>(
            *device_, sizeof(ScreenSpaceLightingUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, "Frame/" + std::to_string(frame) +
                   "/ScreenSpaceLightingUbo");
        screenSpaceUniformBuffers_[frame]->map();
    }
}

void Renderer::createScreenSpaceDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &screenSpaceDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        screenSpaceDescriptorSetLayout_,
        "DescriptorLayout/ScreenSpaceLighting");
}

void Renderer::createScreenSpaceFallback() {
    constexpr std::array<uint8_t, 4> white = {255, 255, 255, 255};
    UploadContext upload(*device_, nullptr, 64 * 1024,
                         "ScreenSpaceFallback");
    TextureCreateInfo info{};
    info.pixels = white.data();
    info.dataSize = white.size();
    info.width = 1;
    info.height = 1;
    info.generateMipmaps = false;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.wrapU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.wrapV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.debugName = "ScreenSpace/Fallback/WhiteAO";
    screenSpaceWhiteFallback_ =
        std::make_shared<Texture>(*device_, upload, info);
    upload.finish();
}

void Renderer::createScreenSpaceDescriptorSets() {
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        screenSpaceDescriptorSets_[frame] = descriptorAllocator_->allocate(
            screenSpaceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
            "Frame/" + std::to_string(frame) +
                "/ScreenSpaceDescriptorSet");
        updateScreenSpaceDescriptor(frame, AmbientOcclusionMode::Off);
    }
}

void Renderer::updateScreenSpaceDescriptor(uint32_t frameIndex,
                                           AmbientOcclusionMode mode) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = screenSpaceUniformBuffers_.at(frameIndex)->handle();
    bufferInfo.range = sizeof(ScreenSpaceLightingUbo);

    VkDescriptorImageInfo imageInfo{};
    if (mode == AmbientOcclusionMode::Ssao &&
        resourceHandles_.ssaoFiltered.valid()) {
        imageInfo.sampler =
            renderResources_->sampler(resourceHandles_.ssaoSampler);
        imageInfo.imageView =
            renderResources_
                ->image(resourceHandles_.ssaoFiltered, frameIndex)
                .imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else if (mode == AmbientOcclusionMode::Cacao &&
               resourceHandles_.cacaoOutput.valid()) {
        imageInfo.sampler =
            renderResources_->sampler(resourceHandles_.ssaoSampler);
        imageInfo.imageView =
            renderResources_->image(resourceHandles_.cacaoOutput, frameIndex)
                .imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else if (mode == AmbientOcclusionMode::Gtao &&
               resourceHandles_.gtaoFiltered.valid()) {
        imageInfo.sampler =
            renderResources_->sampler(resourceHandles_.ssaoSampler);
        imageInfo.imageView =
            renderResources_->image(resourceHandles_.gtaoFiltered, frameIndex)
                .imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        imageInfo.sampler = screenSpaceWhiteFallback_->sampler();
        imageInfo.imageView = screenSpaceWhiteFallback_->imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = screenSpaceDescriptorSets_.at(frameIndex);
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = screenSpaceDescriptorSets_.at(frameIndex);
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
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
    const GpuVisibilityDrawStream &stream =
        occlusionCullPass_->drawStream(lastOcclusionFrameIndex_);
    status.latestCandidates = stream.candidateCount;
    status.latestUncullable =
        lastOcclusionRequested_ > stream.candidateCount
            ? lastOcclusionRequested_ - stream.candidateCount
            : 0;
    status.completed = occlusionCullPass_->completedStatistics();
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

SurfaceDataStatus Renderer::surfaceDataStatus() const {
    SurfaceDataStatus status{};
    const SurfaceDataSupport &support = device_->surfaceDataSupport();
    status.supported = support.available;
    status.active = support.available && lastSurfaceDataActive_;
    status.unavailableReason = support.reason;
    status.depthFormat = support.depthFormat;
    status.normalRoughnessFormat = support.normalRoughnessFormat;
    status.motionFormat = support.motionFormat;
    if (!surfacePrepass_)
        return status;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        status.historyCapacities[frame] =
            surfacePrepass_->historyCapacity(frame);
    }
    status.allocatedBytes = surfacePrepass_->allocatedBytes();
    return status;
}

ScreenSpaceEffectsStatus Renderer::screenSpaceEffectsStatus() const {
    ScreenSpaceEffectsStatus status = screenSpaceStatus_;
    const ScreenSpaceEffectsSupport &support =
        device_->screenSpaceEffectsSupport();
    status.depthPyramidSupported = support.depthPyramidAvailable;
    status.colorPyramidSupported = support.colorPyramidAvailable;
    status.ssaoSupported = support.ssaoAvailable;
    status.gtaoSupported = support.gtaoAvailable;
    status.taaSupported = support.taaAvailable;
    status.ssrSupported = support.ssrAvailable;
    status.ssgiSupported = support.ssgiAvailable;
    status.depthPyramidUnavailableReason = support.depthPyramidReason;
    status.colorPyramidUnavailableReason = support.colorPyramidReason;
    status.ssaoUnavailableReason = support.ssaoReason;
    status.gtaoUnavailableReason = support.gtaoReason;
    status.taaUnavailableReason = support.taaReason;
    status.ssrUnavailableReason = support.ssrReason;
    status.ssgiUnavailableReason = support.ssgiReason;
    const CacaoSupport &cacaoSupport = device_->cacaoSupport();
    status.cacaoCompiled = cacaoSupport.compiled;
    status.cacaoSupported = cacaoSupport.available;
    status.cacaoFp32 = cacaoSupport.fp32;
    status.cacaoUnavailableReason = cacaoSupport.reason;
    if (cacaoPass_) {
        const CacaoRuntimeStatus &cacao = cacaoPass_->status();
        status.cacaoInitialized = cacao.initialized;
        status.cacaoInternalMemoryTracked = cacao.internalMemoryTracked;
        status.cacaoOutputExtent = cacao.outputExtent;
        status.cacaoResolution = cacao.resolution;
        status.cacaoGeneration = cacao.generation;
        if (!cacao.unavailableReason.empty())
            status.cacaoUnavailableReason = cacao.unavailableReason;
    }

    const auto mipBytes = [&](RenderImageHandle handle,
                              uint32_t bytesPerPixel) {
        if (!handle.valid())
            return uint64_t{0};
        uint64_t bytes = 0;
        const uint32_t mipCount = renderResources_->mipLevelCount(handle);
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            const VkExtent2D extent = renderResources_->mipExtent(handle, mip);
            bytes += static_cast<uint64_t>(extent.width) * extent.height *
                     bytesPerPixel;
        }
        return bytes * MAX_FRAMES_IN_FLIGHT;
    };
    if (resourceHandles_.screenDepthPyramid.valid()) {
        status.depthMipLevels = renderResources_->mipLevelCount(
            resourceHandles_.screenDepthPyramid);
        status.depthExtent =
            renderResources_->extent(resourceHandles_.screenDepthPyramid);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.screenDepthPyramid, 4);
    }
    if (resourceHandles_.sceneColorPyramid.valid()) {
        status.colorMipLevels = renderResources_->mipLevelCount(
            resourceHandles_.sceneColorPyramid);
        status.colorExtent =
            renderResources_->extent(resourceHandles_.sceneColorPyramid);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.sceneColorPyramid, 8);
    }
    if (resourceHandles_.ssaoRaw.valid()) {
        status.ssaoExtent =
            renderResources_->extent(resourceHandles_.ssaoRaw);
        const uint64_t imageBytes =
            static_cast<uint64_t>(status.ssaoExtent.width) *
            status.ssaoExtent.height * 2u * MAX_FRAMES_IN_FLIGHT;
        status.estimatedMemoryBytes += imageBytes * 3u;
    }
    if (resourceHandles_.cacaoDepth.valid()) {
        const VkExtent2D extent =
            renderResources_->extent(resourceHandles_.cacaoDepth);
        const uint64_t pixels = static_cast<uint64_t>(extent.width) *
                                extent.height * MAX_FRAMES_IN_FLIGHT;
        status.estimatedMemoryBytes += pixels * 4u;
        status.estimatedMemoryBytes += pixels * 4u;
        status.estimatedMemoryBytes += pixels * 4u;
    }
    if (resourceHandles_.gtaoRaw.valid()) {
        status.gtaoExtent =
            renderResources_->extent(resourceHandles_.gtaoRaw);
        const uint64_t imageBytes =
            static_cast<uint64_t>(status.gtaoExtent.width) *
            status.gtaoExtent.height * 2u * MAX_FRAMES_IN_FLIGHT;
        status.estimatedMemoryBytes += imageBytes * 5u;
    }
    if (resourceHandles_.taaHistory.valid()) {
        status.taaExtent =
            renderResources_->extent(resourceHandles_.taaHistory);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.taaHistory, 8);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.taaDebug, 8);
    }
    if (resourceHandles_.ssrRaw.valid()) {
        status.ssrExtent = renderResources_->extent(resourceHandles_.ssrRaw);
        const uint64_t imageBytes =
            static_cast<uint64_t>(status.ssrExtent.width) *
            status.ssrExtent.height * 8u * MAX_FRAMES_IN_FLIGHT;
        status.estimatedMemoryBytes += imageBytes * 5u;
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.baselineSpecular, 8);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.compositedHdrColor, 8);
    }
    if (resourceHandles_.ssgiRaw.valid()) {
        status.ssgiExtent = renderResources_->extent(resourceHandles_.ssgiRaw);
        const uint64_t imageBytes =
            static_cast<uint64_t>(status.ssgiExtent.width) *
            status.ssgiExtent.height * 8u * MAX_FRAMES_IN_FLIGHT;
        status.estimatedMemoryBytes += imageBytes * 6u;
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.surfaceAlbedoMetallic, 4);
        status.estimatedMemoryBytes +=
            mipBytes(resourceHandles_.baselineDiffuse, 8);
    }
    return status;
}

bool Renderer::reconfigureCacao(CacaoResolution resolution,
                                std::string &error) {
    if (!cacaoPass_) {
        error = device_->cacaoSupport().reason.empty()
                    ? "CACAO is unavailable"
                    : device_->cacaoSupport().reason;
        return false;
    }
    return cacaoPass_->reconfigure(*renderResources_, resolution, error);
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

    if (device_->surfaceDataSupport().available) {
        auto surfacePass = std::make_unique<SurfacePrepass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.surfacePrepassVert,
            shaderPaths_.surfacePrepassOpaqueFrag,
            shaderPaths_.surfacePrepassMaskFrag);
        surfacePrepass_ = surfacePass.get();
        pipeline_.addPass(std::move(surfacePass));
    }
    if (device_->occlusionCullingSupport().available) {
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

    const ScreenSpaceEffectsSupport &screenSupport =
        device_->screenSpaceEffectsSupport();
    if (screenSupport.depthPyramidAvailable) {
        pipeline_.addPass(std::make_unique<ScreenSpacePyramidPass>(
            *device_, *renderResources_,
            ScreenSpacePyramidKind::NearestDepth,
            resourceHandles_.surfaceDepth,
            resourceHandles_.surfaceDepthSampler,
            RenderImageHandle{}, RenderSamplerHandle{},
            resourceHandles_.screenDepthPyramid,
            resourceHandles_.screenPyramidSampler, *descriptorAllocator_,
            shaderPaths_.screenDepthInitComp,
            shaderPaths_.screenDepthReduceComp));
    }
    if (screenSupport.ssaoAvailable) {
        pipeline_.addPass(std::make_unique<SsaoPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.ssaoTraceComp, shaderPaths_.ssaoBlurComp));
    }
    if (screenSupport.gtaoAvailable) {
        auto gtaoPass = std::make_unique<GtaoPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.gtaoTraceComp, shaderPaths_.gtaoTemporalComp,
            shaderPaths_.ssaoBlurComp);
        gtaoPass_ = gtaoPass.get();
        pipeline_.addPass(std::move(gtaoPass));
    }
    if (device_->cacaoSupport().available) {
        pipeline_.addPass(std::make_unique<CacaoNormalAdapterPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.cacaoNormalAdapterComp));
        auto cacaoPass = std::make_unique<CacaoPass>(
            *device_, *renderResources_, resourceHandles_,
            CacaoResolution::Half);
        cacaoPass_ = cacaoPass.get();
        pipeline_.addPass(std::move(cacaoPass));
    }

    pipeline_.addPass(std::make_unique<SkyBackgroundPass>(
        *device_, *renderResources_, resourceHandles_,
        globalDescriptorSetLayout_, lightingDescriptorSetLayout_,
        atmosphereDescriptorSetLayout_, shaderPaths_.fullscreenVert,
        shaderPaths_.skyboxFrag, shaderPaths_.atmosphereSkyFrag));

    auto mainPass = std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_,
        ForwardPhase::Opaque,
        lightingDescriptorSetLayout_, atmosphereDescriptorSetLayout_);
    mainForwardPass_ = mainPass.get();
    pipeline_.addPass(std::move(mainPass));

    if (screenSupport.colorPyramidAvailable) {
        pipeline_.addPass(std::make_unique<ScreenSpacePyramidPass>(
            *device_, *renderResources_, ScreenSpacePyramidKind::SceneColor,
            resourceHandles_.hdrColor, resourceHandles_.hdrSampler,
            RenderImageHandle{}, RenderSamplerHandle{},
            resourceHandles_.sceneColorPyramid,
            resourceHandles_.screenPyramidSampler, *descriptorAllocator_,
            shaderPaths_.screenColorInitComp,
            shaderPaths_.screenColorReduceComp));
    }

    if (screenSupport.ssrAvailable) {
        auto ssrPass = std::make_unique<SsrPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.ssrTraceComp, shaderPaths_.ssrTemporalComp,
            shaderPaths_.ssrBlurComp);
        ssrPass_ = ssrPass.get();
        pipeline_.addPass(std::move(ssrPass));
    }

    if (screenSupport.ssgiAvailable) {
        auto ssgiPass = std::make_unique<SsgiPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            shaderPaths_.ssgiTraceComp, shaderPaths_.ssgiTemporalComp,
            shaderPaths_.ssgiFilterComp);
        ssgiPass_ = ssgiPass.get();
        pipeline_.addPass(std::move(ssgiPass));
    }

    RendererResourceHandles postProcessHandles = resourceHandles_;
    pipeline_.addPass(std::make_unique<HdrCompositePass>(
        *device_, *renderResources_, resourceHandles_,
        *descriptorAllocator_, shaderPaths_.reflectionCompositeComp));
    pipeline_.addPass(std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_,
        ForwardPhase::Transparent,
        lightingDescriptorSetLayout_, atmosphereDescriptorSetLayout_));
    postProcessHandles.hdrColor = resourceHandles_.compositedHdrColor;

    if (screenSupport.taaAvailable) {
        auto taaPass = std::make_unique<TaaPass>(
            *device_, *renderResources_, postProcessHandles,
            *descriptorAllocator_, shaderPaths_.taaResolveComp);
        taaPass_ = taaPass.get();
        pipeline_.addPass(std::move(taaPass));
    }

    if (device_->computeBloomSupport().available) {
        pipeline_.addPass(std::make_unique<BloomPass>(
            *device_, *renderResources_, postProcessHandles,
            *descriptorAllocator_, shaderPaths_.bloomDownsampleComp,
            shaderPaths_.bloomUpsampleComp));
    }

    auto toneMapPass = std::make_unique<ToneMapPass>(
        *device_, *renderResources_, postProcessHandles.hdrColor,
        resourceHandles_.hdrSampler, resourceHandles_.bloomLevels.front(),
        resourceHandles_.bloomSampler, resourceHandles_.viewportColor,
        resourceHandles_.surfaceNormalRoughness,
        resourceHandles_.surfaceMotion,
        resourceHandles_.surfaceDataSampler,
        resourceHandles_.screenDepthPyramid,
        resourceHandles_.sceneColorPyramid,
        resourceHandles_.ssaoRaw,
        resourceHandles_.ssaoFiltered,
        resourceHandles_.cacaoOutput,
        resourceHandles_.gtaoRaw,
        resourceHandles_.gtaoHistory,
        resourceHandles_.gtaoFiltered,
        resourceHandles_.gtaoDebug,
        resourceHandles_.taaHistory,
        resourceHandles_.taaDebug,
        resourceHandles_.ssrRaw,
        resourceHandles_.ssrHistory,
        resourceHandles_.ssrFiltered,
        resourceHandles_.ssrDebug,
        resourceHandles_.ssgiRaw,
        resourceHandles_.ssgiHistory,
        resourceHandles_.ssgiFiltered,
        resourceHandles_.ssgiDebug,
        resourceHandles_.screenPyramidSampler,
        resourceHandles_.ssaoSampler,
        resourceHandles_.taaSampler,
        resourceHandles_.ssrSampler,
        resourceHandles_.ssgiSampler,
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
