add_library(vkl_shader_catalog STATIC
    shader/ShaderRegistry.cpp
)
add_library(VulkanLab::ShaderCatalog ALIAS vkl_shader_catalog)
target_include_directories(vkl_shader_catalog PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_shader_catalog
    PUBLIC
        vkl_build_options
    PRIVATE
        vkl_json
)

set(VKL_RENDER_FRAME_SOURCES
    frame/FrameFeatureResolver.cpp
    frame/RenderSettingsController.cpp
    frame/RenderView.cpp
)

set(VKL_RENDER_GRAPH_SOURCES
    graph/FrameGraphExternalPasses.cpp
    graph/RenderGraph.cpp
    graph/RenderResourcePool.cpp
)

set(VKL_RENDER_GEOMETRY_SOURCES
    geometry/BoundsMath.cpp
    geometry/Mesh.cpp
    geometry/RenderQueue.cpp
    geometry/TangentGenerator.cpp
)

set(VKL_RENDER_MATERIAL_SOURCES
    material/FallbackTextures.cpp
    material/MaterialInstance.cpp
    material/MaterialSystem.cpp
    material/MaterialTemplate.cpp
    material/Texture.cpp
    material/TextureData.cpp
)

set(VKL_RENDER_PIPELINE_SOURCES
    pipeline/ComputePipeline.cpp
    pipeline/Pipeline.cpp
    pipeline/PipelineCache.cpp
    pipeline/PipelineConfigBuilder.cpp
)

set(VKL_RENDER_SHADER_SOURCES
    shader/RendererProgramCatalog.cpp
)

set(VKL_RENDER_CORE_FORWARD_SOURCES
    features/forward/MainForwardPass.cpp
    features/post_process/HdrCompositePass.cpp
    features/post_process/PresentPass.cpp
    features/post_process/ToneMapPass.cpp
)

set(VKL_RENDER_SURFACE_SOURCES
    features/surface/DepthHierarchyPass.cpp
    features/surface/GBufferPass.cpp
    features/surface/SurfaceFrameData.cpp
    features/surface/SurfaceDrawRecorder.cpp
    features/surface/SurfacePrepass.cpp
)

set(VKL_RENDER_DEFERRED_SOURCES
    features/deferred/DeferredLightingPass.cpp
)

set(VKL_RENDER_LIGHTING_SOURCES
    features/lighting/ClusteredLighting.cpp
    features/lighting/ClusteredLightCullingPass.cpp
)

set(VKL_RENDER_SHADOW_VISIBILITY_SOURCES
    features/shadows_visibility/DirectionalShadow.cpp
    features/shadows_visibility/DirectionalShadowPass.cpp
    features/shadows_visibility/OcclusionCullPass.cpp
    features/shadows_visibility/PointShadowPass.cpp
    features/shadows_visibility/PunctualShadow.cpp
    features/shadows_visibility/PunctualShadowSliceBuffer.cpp
    features/shadows_visibility/ShadowCasterDrawRecorder.cpp
    features/shadows_visibility/ShadowSystem.cpp
    features/shadows_visibility/ShadowVisibilityBuilder.cpp
    features/shadows_visibility/SpotShadowPass.cpp
    features/shadows_visibility/Visibility.cpp
)

set(VKL_RENDER_AO_SOURCES
    features/ambient_occlusion/CacaoNormalAdapterPass.cpp
    features/ambient_occlusion/GtaoPass.cpp
    features/ambient_occlusion/SsaoPass.cpp
)
if(VKL_ENABLE_CACAO)
    list(APPEND VKL_RENDER_AO_SOURCES
        features/ambient_occlusion/CacaoPass.cpp)
else()
    list(APPEND VKL_RENDER_AO_SOURCES
        features/ambient_occlusion/CacaoPassDisabled.cpp)
endif()

set(VKL_RENDER_REFLECTION_SOURCES
    features/reflections/SsrPass.cpp
)

set(VKL_RENDER_GI_SOURCES
    features/global_illumination/DdgiPass.cpp
    features/global_illumination/RayTracingScene.cpp
    features/global_illumination/SsgiPass.cpp
)

set(VKL_RENDER_ATMOSPHERE_ENVIRONMENT_SOURCES
    features/atmosphere_environment/AtmosphereLutPass.cpp
    features/atmosphere_environment/SkyBackgroundPass.cpp
)

set(VKL_RENDER_TEMPORAL_POSTPROCESS_SOURCES
    features/temporal_post_process/BloomPass.cpp
    features/temporal_post_process/ScreenSpacePyramidPass.cpp
    features/temporal_post_process/TaaPass.cpp
    features/temporal_post_process/TemporalAA.cpp
)

set(VKL_RENDERER_SOURCES
    Renderer.cpp
    RendererFeatureGraph.cpp
    ${VKL_RENDER_FRAME_SOURCES}
    ${VKL_RENDER_GRAPH_SOURCES}
    ${VKL_RENDER_GEOMETRY_SOURCES}
    ${VKL_RENDER_MATERIAL_SOURCES}
    ${VKL_RENDER_PIPELINE_SOURCES}
    ${VKL_RENDER_SHADER_SOURCES}
    ${VKL_RENDER_CORE_FORWARD_SOURCES}
    ${VKL_RENDER_SURFACE_SOURCES}
    ${VKL_RENDER_DEFERRED_SOURCES}
    ${VKL_RENDER_LIGHTING_SOURCES}
    ${VKL_RENDER_SHADOW_VISIBILITY_SOURCES}
    ${VKL_RENDER_AO_SOURCES}
    ${VKL_RENDER_REFLECTION_SOURCES}
    ${VKL_RENDER_GI_SOURCES}
    ${VKL_RENDER_ATMOSPHERE_ENVIRONMENT_SOURCES}
    ${VKL_RENDER_TEMPORAL_POSTPROCESS_SOURCES}
)
if(VKL_ENABLE_GPU_PROFILING)
    list(APPEND VKL_RENDERER_SOURCES diagnostics/GpuPassProfiler.cpp)
else()
    list(APPEND VKL_RENDERER_SOURCES diagnostics/GpuPassProfilerDisabled.cpp)
endif()

add_library(vkl_renderer_runtime STATIC ${VKL_RENDERER_SOURCES})
add_library(VulkanLab::RendererRuntime ALIAS vkl_renderer_runtime)
target_include_directories(vkl_renderer_runtime PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_renderer_runtime
    PUBLIC
        vkl_build_options
        vkl_gpu_runtime
        vkl_shader_catalog
        vkl_scene_data
        vkl_glm
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        vkl_image_codecs
        vkl_cacao
        vkl_tracy
)
