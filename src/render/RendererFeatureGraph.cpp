#include "render/Renderer.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "render/features/global_illumination/RayTracingScene.h"
#include "render/features/atmosphere_environment/AtmosphereLutPass.h"
#include "render/features/temporal_post_process/BloomPass.h"
#include "render/features/ambient_occlusion/CacaoNormalAdapterPass.h"
#include "render/features/ambient_occlusion/CacaoPass.h"
#include "render/features/global_illumination/DdgiPass.h"
#include "render/features/shadows_visibility/DirectionalShadowPass.h"
#include "render/graph/FrameGraphExternalPasses.h"
#include "render/features/ambient_occlusion/GtaoPass.h"
#include "render/features/post_process/HdrCompositePass.h"
#include "render/features/deferred/DeferredLightingPass.h"
#include "render/features/lighting/ClusteredLightCullingPass.h"
#include "render/features/lighting/ClusteredLighting.h"
#include "render/features/deferred/DeferredLightingResources.h"
#include "render/features/surface/DepthHierarchyPass.h"
#include "render/features/surface/DepthHierarchyResources.h"
#include "render/features/surface/GBufferPass.h"
#include "render/features/surface/GBufferResources.h"
#include "render/features/surface/SurfaceFrameData.h"
#include "render/features/forward/MainForwardPass.h"
#include "render/features/shadows_visibility/OcclusionCullPass.h"
#include "render/features/shadows_visibility/PointShadowPass.h"
#include "render/features/post_process/PresentPass.h"
#include "render/features/temporal_post_process/ScreenSpacePyramidPass.h"
#include "render/features/atmosphere_environment/SkyBackgroundPass.h"
#include "render/features/shadows_visibility/SpotShadowPass.h"
#include "render/features/ambient_occlusion/SsaoPass.h"
#include "render/features/global_illumination/SsgiPass.h"
#include "render/features/reflections/SsrPass.h"
#include "render/features/surface/SurfacePrepass.h"
#include "render/features/temporal_post_process/TaaPass.h"
#include "render/features/post_process/ToneMapPass.h"

namespace vkr {

void Renderer::registerAtmosphereShadowFeatures() {
    const AtmosphereGiPrograms &atmosphere = programs_.atmosphereGi;
    if (device_->atmosphereSupport().available) {
        auto pass = std::make_unique<AtmosphereLutPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, atmosphereDescriptorSetLayout_,
            atmosphere.transmittanceCompute,
            atmosphere.multipleScatteringCompute,
            atmosphere.skyViewCompute,
            atmosphere.aerialPerspectiveCompute);
        atmosphereLutPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }

    const ShadowPrograms &shadows = programs_.shadows;
    renderGraph_.addPass(std::make_unique<DirectionalShadowPass>(
        *device_, *renderResources_, resourceHandles_.directionalShadowDepth,
        globalDescriptorSetLayout_, shadows.directionalVertex));
    renderGraph_.addPass(std::make_unique<PointShadowPass>(
        *device_, *renderResources_,
        resourceHandles_.pointShadowDepthByCapacity, *descriptorAllocator_,
        shadows.punctualVertex, shadows.pointFragment));
    renderGraph_.addPass(std::make_unique<SpotShadowPass>(
        *device_, *renderResources_,
        resourceHandles_.spotShadowDepthByCapacity, *descriptorAllocator_,
        shadows.punctualVertex));
}

void Renderer::registerSurfaceVisibilityFeatures() {
    const SurfaceVisibilityPrograms &programs = programs_.surfaceVisibility;
    if (device_->surfaceDataSupport().available) {
        surfaceFrameData_ = std::make_unique<SurfaceFrameData>(
            *device_, *descriptorAllocator_);
        auto pass = std::make_unique<SurfacePrepass>(
            *device_, *renderResources_, resourceHandles_,
            *surfaceFrameData_, globalDescriptorSetLayout_);
        surfacePrepass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }
    const DepthHierarchyResources hierarchy =
        depthHierarchyResources(resourceHandles_);
    if (device_->depthHierarchySupport().available()) {
        const ScreenSpacePrograms &screen = programs_.screenSpace;
        renderGraph_.addPass(std::make_unique<DepthHierarchyPass>(
            *device_, *renderResources_, resourceHandles_.surfaceDepth,
            resourceHandles_.surfaceDepthSampler, hierarchy,
            *descriptorAllocator_,
            DepthHierarchyPrograms{
                programs.depthHierarchyInitCompute,
                programs.depthHierarchyReduceCompute,
                screen.depthInitCompute, screen.depthReduceCompute,
                programs.hiZInitCompute, programs.hiZReduceCompute}));
    }
    if (device_->occlusionCullingSupport().available) {
        auto occlusion = std::make_unique<OcclusionCullPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, programs.occlusionCompute);
        occlusionCullPass_ = occlusion.get();
        renderGraph_.addPass(std::move(occlusion));
    }
    if (device_->gBufferSupport().available && surfaceFrameData_) {
        auto gBuffer = std::make_unique<GBufferPass>(
            *device_, gBufferResources(resourceHandles_),
            *surfaceFrameData_);
        gBufferPass_ = gBuffer.get();
        renderGraph_.addPass(std::move(gBuffer));
    }
}

void Renderer::registerIndirectLightingPreparationFeatures() {
    const ScreenSpaceEffectsSupport &support =
        device_->screenSpaceEffectsSupport();
    const ScreenSpacePrograms &screen = programs_.screenSpace;
    if (support.ssaoAvailable) {
        renderGraph_.addPass(std::make_unique<SsaoPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            screen.ssaoTraceCompute, screen.ssaoBlurCompute));
    }
    if (support.gtaoAvailable) {
        auto pass = std::make_unique<GtaoPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            screen.gtaoTraceCompute, screen.gtaoTemporalCompute,
            screen.ssaoBlurCompute);
        gtaoPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }
    if (device_->cacaoSupport().available) {
        renderGraph_.addPass(std::make_unique<CacaoNormalAdapterPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            screen.cacaoNormalAdapterCompute));
        auto pass = std::make_unique<CacaoPass>(
            *device_, *renderResources_, resourceHandles_,
            CacaoResolution::Half);
        cacaoPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }

    const AtmosphereGiPrograms &gi = programs_.atmosphereGi;
    auto ddgi = std::make_unique<DdgiPass>(
        *device_, *renderResources_, resourceHandles_, *descriptorAllocator_,
        globalDescriptorSetLayout_, *rayTracingScene_, gi.ddgiTraceCompute,
        gi.ddgiUpdateCompute);
    ddgiPass_ = ddgi.get();
    renderGraph_.addPass(
        std::make_unique<RayTracingSceneBuildPass>(*rayTracingScene_));
    renderGraph_.addPass(std::move(ddgi));
}

void Renderer::registerSceneLightingFeatures() {
    const PostProcessPrograms &post = programs_.postProcess;
    renderGraph_.addPass(std::make_unique<ClusteredLightCullingPass>(
        *device_, *clusteredLighting_, globalDescriptorSetLayout_,
        programs_.deferred.clusterBuildCompute));
    renderGraph_.addPass(std::make_unique<SkyBackgroundPass>(
        *device_, *renderResources_, resourceHandles_,
        globalDescriptorSetLayout_, lightingDescriptorSetLayout_,
        atmosphereDescriptorSetLayout_, post.fullscreenVertex,
        post.skyboxFragment,
        programs_.atmosphereGi.atmosphereSkyFragment));

    auto forward = std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_, ForwardPhase::Opaque,
        lightingDescriptorSetLayout_, atmosphereDescriptorSetLayout_,
        ddgiPass_->samplingDescriptorSetLayout(), *clusteredLighting_);
    mainForwardPass_ = forward.get();
    renderGraph_.addPass(std::move(forward));

    if (device_->gBufferSupport().available &&
        device_->computeBloomSupport().available) {
        auto deferred = std::make_unique<DeferredLightingPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            lightingDescriptorSetLayout_, atmosphereDescriptorSetLayout_,
            screenSpaceDescriptorSetLayout_,
            ddgiPass_->samplingDescriptorSetLayout(),
            *clusteredLighting_,
            programs_.deferred.lightingCompute);
        deferredLightingPass_ = deferred.get();
        renderGraph_.addPass(std::move(deferred));
    }

    const ScreenSpaceEffectsSupport &support =
        device_->screenSpaceEffectsSupport();
    const ScreenSpacePrograms &screen = programs_.screenSpace;
    if (support.colorPyramidAvailable) {
        renderGraph_.addPass(std::make_unique<ScreenSpacePyramidPass>(
            *device_, *renderResources_,
            resourceHandles_.hdrColor, resourceHandles_.hdrSampler,
            resourceHandles_.deferredHdrColor,
            resourceHandles_.hdrSampler,
            resourceHandles_.sceneColorPyramid,
            resourceHandles_.screenPyramidSampler, *descriptorAllocator_,
            screen.colorInitCompute, screen.colorReduceCompute));
    }
    if (support.ssrAvailable) {
        auto pass = std::make_unique<SsrPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            screen.ssrTraceCompute, screen.ssrTemporalCompute,
            screen.ssrBlurCompute);
        ssrPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }
    if (support.ssgiAvailable) {
        auto pass = std::make_unique<SsgiPass>(
            *device_, *renderResources_, resourceHandles_,
            *descriptorAllocator_, globalDescriptorSetLayout_,
            screen.ssgiTraceCompute, screen.ssgiTemporalCompute,
            screen.ssgiFilterCompute);
        ssgiPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }

    renderGraph_.addPass(std::make_unique<HdrCompositePass>(
        *device_, *renderResources_, resourceHandles_, *descriptorAllocator_,
        screen.lightingCompositeCompute));
    renderGraph_.addPass(std::make_unique<MainForwardPass>(
        *device_, *renderResources_, resourceHandles_,
        ForwardPhase::Transparent, lightingDescriptorSetLayout_,
        atmosphereDescriptorSetLayout_,
        ddgiPass_->samplingDescriptorSetLayout(), *clusteredLighting_));
}

void Renderer::registerPostProcessFeatures() {
    const ScreenSpaceEffectsSupport &support =
        device_->screenSpaceEffectsSupport();
    RendererResourceHandles postProcessHandles = resourceHandles_;
    if (support.taaAvailable) {
        auto pass = std::make_unique<TaaPass>(
            *device_, *renderResources_, postProcessHandles,
            *descriptorAllocator_, programs_.screenSpace.taaResolveCompute);
        taaPass_ = pass.get();
        renderGraph_.addPass(std::move(pass));
    }

    const PostProcessPrograms &post = programs_.postProcess;
    if (device_->computeBloomSupport().available) {
        renderGraph_.addPass(std::make_unique<BloomPass>(
            *device_, *renderResources_, postProcessHandles,
            *descriptorAllocator_, post.bloomDownsampleCompute,
            post.bloomUpsampleCompute));
    }

    auto toneMap = std::make_unique<ToneMapPass>(
        *device_, *renderResources_, postProcessHandles.hdrColor,
        resourceHandles_.compositedHdrColor, resourceHandles_.hdrSampler,
        resourceHandles_.bloomLevels.front(), resourceHandles_.bloomSampler,
        resourceHandles_.viewportColor,
        resourceHandles_.surfaceNormalRoughness,
        resourceHandles_.surfaceMotion, resourceHandles_.surfaceDataSampler,
        gBufferResources(resourceHandles_),
        deferredLightingResources(resourceHandles_),
        depthHierarchyResources(resourceHandles_)
            .image(DepthHierarchySemantic::Nearest),
        resourceHandles_.sceneColorPyramid, resourceHandles_.ssaoRaw,
        resourceHandles_.ssaoFiltered, resourceHandles_.cacaoOutput,
        resourceHandles_.gtaoRaw, resourceHandles_.gtaoHistory,
        resourceHandles_.gtaoFiltered, resourceHandles_.gtaoDebug,
        resourceHandles_.taaHistory, resourceHandles_.taaDebug,
        resourceHandles_.ssrRaw, resourceHandles_.ssrHistory,
        resourceHandles_.ssrFiltered, resourceHandles_.ssrDebug,
        resourceHandles_.ssgiRaw, resourceHandles_.ssgiHistory,
        resourceHandles_.ssgiFiltered, resourceHandles_.ssgiDebug,
        resourceHandles_.screenPyramidSampler, resourceHandles_.ssaoSampler,
        resourceHandles_.taaSampler, resourceHandles_.ssrSampler,
        resourceHandles_.ssgiSampler, *descriptorAllocator_,
        post.fullscreenVertex, post.toneMapFragment);
    toneMapPass_ = toneMap.get();
    renderGraph_.addPass(std::move(toneMap));

    auto present = std::make_unique<PresentPass>(
        *device_, *swapChain_, *renderResources_,
        resourceHandles_.viewportColor, resourceHandles_.viewportSampler,
        *descriptorAllocator_, post.fullscreenVertex, post.presentFragment);
    presentPass_ = present.get();
    renderGraph_.addPass(std::move(present));
    renderGraph_.addPass(
        std::make_unique<ScreenshotCopyPass>(resourceHandles_));
}

void Renderer::createRenderGraph() {
    registerAtmosphereShadowFeatures();
    registerSurfaceVisibilityFeatures();
    registerIndirectLightingPreparationFeatures();
    registerSceneLightingFeatures();
    registerPostProcessFeatures();
}

} // namespace vkr
