add_library(vkl_foundation STATIC
    core/Log.cpp
    diagnostics/BuildInfo.cpp
    diagnostics/DiagnosticsConfig.cpp
    diagnostics/SubmissionSerialTracker.cpp
)
add_library(VulkanLab::Foundation ALIAS vkl_foundation)
target_include_directories(vkl_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(vkl_foundation PRIVATE
    "${VKL_BUILD_INFO_INCLUDE_DIR}"
)
add_dependencies(vkl_foundation VulkanLabBuildInfo)
target_link_libraries(vkl_foundation PUBLIC
    vkl_build_options
    vkl_spdlog
)

add_library(vkl_shader_catalog STATIC
    render/shader/ShaderRegistry.cpp
)
add_library(VulkanLab::ShaderCatalog ALIAS vkl_shader_catalog)
target_include_directories(vkl_shader_catalog PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(vkl_shader_catalog
    PUBLIC
        vkl_build_options
    PRIVATE
        vkl_json
)

add_library(vkl_scene_data STATIC
    scene_data/PrimitiveModelDefinitions.cpp
    scene_data/SceneDocument.cpp
    scene_data/SceneIds.cpp
)
add_library(VulkanLab::SceneData ALIAS vkl_scene_data)
target_include_directories(vkl_scene_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_scene_data
    PUBLIC
        vkl_build_options
        vkl_glm
    PRIVATE
        vkl_json
        bcrypt
)

set(VKL_ASSET_CORE_SOURCES
    assets/AssetValidation.cpp
    assets/ArtifactCachePruner.cpp
    assets/ArtifactIndex.cpp
    assets/ArtifactStatus.cpp
    assets/CacheMutationLock.cpp
    assets/ContentHash.cpp
    assets/DerivedAssetPaths.cpp
    assets/DerivedEnvironmentManifest.cpp
    assets/DerivedTextureManifest.cpp
    assets/ProjectContext.cpp
    assets/RuntimePackage.cpp
    assets/SceneCatalog.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING OR VKL_BUILD_ASSET_TOOL OR BUILD_TESTING)
    list(APPEND VKL_ASSET_CORE_SOURCES
        assets/SceneCatalogEditor.cpp
        assets/SceneCatalogStore.cpp
        assets/ModelImportService.cpp
    )
endif()

add_library(vkl_asset_core STATIC ${VKL_ASSET_CORE_SOURCES})
add_library(VulkanLab::AssetCore ALIAS vkl_asset_core)
target_include_directories(vkl_asset_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_asset_core
    PUBLIC
        vkl_build_options
        vkl_glm
        vkl_scene_data
    PRIVATE
        vkl_json
        bcrypt
)

set(VKL_CONTROL_REQUIRED OFF)
if(VKL_ENABLE_RUNTIME_CONTROL OR VKL_BUILD_CONTROL_TOOL OR
   VKL_BUILD_RENDER_TEST OR BUILD_TESTING)
    set(VKL_CONTROL_REQUIRED ON)
endif()

if(VKL_CONTROL_REQUIRED)
    add_library(vkl_control STATIC
        control/NamedPipeServerWin32.cpp
        control/RuntimeControlClientWin32.cpp
        control/RuntimeCommand.cpp
        control/RuntimeCommandDispatcher.cpp
        control/RuntimeControlProtocol.cpp
    )
    add_library(VulkanLab::Control ALIAS vkl_control)
    target_include_directories(vkl_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(vkl_control
        PUBLIC
            vkl_build_options
            vkl_json
        PRIVATE
            vkl_foundation
    )
endif()

set(VKL_GPU_RUNTIME_SOURCES
    core/AccelerationStructure.cpp
    core/Buffer.cpp
    core/DescriptorAllocator.cpp
    core/Device.cpp
    core/FrameSync.cpp
    core/GpuBarrier.h
    core/Image.cpp
    core/IncrementalUploadQueue.cpp
    core/SwapChain.cpp
    core/UploadContext.cpp
    core/ValidationProfile.cpp
    core/VulkanContext.cpp
    core/VulkanException.cpp
)
if(VKL_ENABLE_GPU_DEBUG_UTILS)
    list(APPEND VKL_GPU_RUNTIME_SOURCES core/GpuDebugUtils.cpp)
else()
    list(APPEND VKL_GPU_RUNTIME_SOURCES core/GpuDebugUtilsDisabled.cpp)
endif()
if(VKL_ENABLE_TRACY)
    list(APPEND VKL_GPU_RUNTIME_SOURCES diagnostics/TracyProfiler.cpp)
else()
    list(APPEND VKL_GPU_RUNTIME_SOURCES diagnostics/TracyProfilerDisabled.cpp)
endif()

add_library(vkl_gpu_runtime STATIC ${VKL_GPU_RUNTIME_SOURCES})
add_library(VulkanLab::GpuRuntime ALIAS vkl_gpu_runtime)
target_include_directories(vkl_gpu_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_gpu_runtime
    PUBLIC
        vkl_build_options
        vkl_foundation
        vkl_glm
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_tracy
        vkl_vma_impl
)

set(VKL_CAPTURE_SOURCES
    diagnostics/CaptureTypes.cpp
    diagnostics/CaptureTaskQueue.cpp
)
if(VKL_ENABLE_CAPTURE)
    list(APPEND VKL_CAPTURE_SOURCES diagnostics/CaptureService.cpp)
else()
    list(APPEND VKL_CAPTURE_SOURCES diagnostics/CaptureServiceDisabled.cpp)
endif()

add_library(vkl_capture STATIC ${VKL_CAPTURE_SOURCES})
add_library(VulkanLab::Capture ALIAS vkl_capture)
target_include_directories(vkl_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_capture
    PUBLIC
        vkl_build_options
        vkl_gpu_runtime
    PRIVATE
        vkl_asset_core
        vkl_foundation
        vkl_image_codecs
        vkl_tracy
)

set(VKL_RENDER_FRAME_SOURCES
    render/frame/FrameFeatureResolver.cpp
    render/frame/RenderSettingsController.cpp
    render/frame/RenderView.cpp
)

set(VKL_RENDER_GRAPH_SOURCES
    render/graph/FrameGraphExternalPasses.cpp
    render/graph/RenderGraph.cpp
    render/graph/RenderResourcePool.cpp
)

set(VKL_RENDER_GEOMETRY_SOURCES
    render/geometry/BoundsMath.cpp
    render/geometry/Mesh.cpp
    render/geometry/RenderQueue.cpp
    render/geometry/TangentGenerator.cpp
)

set(VKL_RENDER_MATERIAL_SOURCES
    render/material/FallbackTextures.cpp
    render/material/MaterialInstance.cpp
    render/material/MaterialSystem.cpp
    render/material/MaterialTemplate.cpp
    render/material/Texture.cpp
    render/material/TextureData.cpp
)

set(VKL_RENDER_PIPELINE_SOURCES
    render/pipeline/ComputePipeline.cpp
    render/pipeline/Pipeline.cpp
    render/pipeline/PipelineCache.cpp
    render/pipeline/PipelineConfigBuilder.cpp
)

set(VKL_RENDER_SHADER_SOURCES
    render/shader/RendererProgramCatalog.cpp
)

set(VKL_RENDER_CORE_FORWARD_SOURCES
    render/features/core_forward/HdrCompositePass.cpp
    render/features/core_forward/MainForwardPass.cpp
    render/features/core_forward/PresentPass.cpp
    render/features/core_forward/SurfacePrepass.cpp
    render/features/core_forward/ToneMapPass.cpp
)

set(VKL_RENDER_SHADOW_VISIBILITY_SOURCES
    render/features/shadows_visibility/DirectionalShadow.cpp
    render/features/shadows_visibility/DirectionalShadowPass.cpp
    render/features/shadows_visibility/HiZBuildPass.cpp
    render/features/shadows_visibility/OcclusionCullPass.cpp
    render/features/shadows_visibility/PointShadowPass.cpp
    render/features/shadows_visibility/PunctualShadow.cpp
    render/features/shadows_visibility/PunctualShadowSliceBuffer.cpp
    render/features/shadows_visibility/ShadowCasterDrawRecorder.cpp
    render/features/shadows_visibility/ShadowSystem.cpp
    render/features/shadows_visibility/ShadowVisibilityBuilder.cpp
    render/features/shadows_visibility/SpotShadowPass.cpp
    render/features/shadows_visibility/Visibility.cpp
)

set(VKL_RENDER_AO_SOURCES
    render/features/ambient_occlusion/CacaoNormalAdapterPass.cpp
    render/features/ambient_occlusion/GtaoPass.cpp
    render/features/ambient_occlusion/SsaoPass.cpp
)
if(VKL_ENABLE_CACAO)
    list(APPEND VKL_RENDER_AO_SOURCES
        render/features/ambient_occlusion/CacaoPass.cpp)
else()
    list(APPEND VKL_RENDER_AO_SOURCES
        render/features/ambient_occlusion/CacaoPassDisabled.cpp)
endif()

set(VKL_RENDER_REFLECTION_SOURCES
    render/features/reflections/SsrPass.cpp
)

set(VKL_RENDER_GI_SOURCES
    render/features/global_illumination/DdgiPass.cpp
    render/features/global_illumination/RayTracingScene.cpp
    render/features/global_illumination/SsgiPass.cpp
)

set(VKL_RENDER_ATMOSPHERE_ENVIRONMENT_SOURCES
    render/features/atmosphere_environment/AtmosphereLutPass.cpp
    render/features/atmosphere_environment/SkyBackgroundPass.cpp
)

set(VKL_RENDER_TEMPORAL_POSTPROCESS_SOURCES
    render/features/temporal_post_process/BloomPass.cpp
    render/features/temporal_post_process/ScreenSpacePyramidPass.cpp
    render/features/temporal_post_process/TaaPass.cpp
    render/features/temporal_post_process/TemporalAA.cpp
)

set(VKL_RENDERER_SOURCES
    render/Renderer.cpp
    render/RendererFeatureGraph.cpp
    ${VKL_RENDER_FRAME_SOURCES}
    ${VKL_RENDER_GRAPH_SOURCES}
    ${VKL_RENDER_GEOMETRY_SOURCES}
    ${VKL_RENDER_MATERIAL_SOURCES}
    ${VKL_RENDER_PIPELINE_SOURCES}
    ${VKL_RENDER_SHADER_SOURCES}
    ${VKL_RENDER_CORE_FORWARD_SOURCES}
    ${VKL_RENDER_SHADOW_VISIBILITY_SOURCES}
    ${VKL_RENDER_AO_SOURCES}
    ${VKL_RENDER_REFLECTION_SOURCES}
    ${VKL_RENDER_GI_SOURCES}
    ${VKL_RENDER_ATMOSPHERE_ENVIRONMENT_SOURCES}
    ${VKL_RENDER_TEMPORAL_POSTPROCESS_SOURCES}
)
if(VKL_ENABLE_GPU_PROFILING)
    list(APPEND VKL_RENDERER_SOURCES render/diagnostics/GpuPassProfiler.cpp)
else()
    list(APPEND VKL_RENDERER_SOURCES render/diagnostics/GpuPassProfilerDisabled.cpp)
endif()

add_library(vkl_renderer_runtime STATIC ${VKL_RENDERER_SOURCES})
add_library(VulkanLab::RendererRuntime ALIAS vkl_renderer_runtime)
target_include_directories(vkl_renderer_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
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

set(VKL_ASSET_RUNTIME_SOURCES
    assets/AssetLoadCoordinator.cpp
    assets/DerivedEnvironmentCache.cpp
    assets/DerivedTextureCache.cpp
    assets/EnvironmentLoadManager.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING)
    list(APPEND VKL_ASSET_RUNTIME_SOURCES assets/AssetImportManager.cpp)
else()
    list(APPEND VKL_ASSET_RUNTIME_SOURCES assets/AssetImportManagerDisabled.cpp)
endif()

add_library(vkl_asset_runtime STATIC ${VKL_ASSET_RUNTIME_SOURCES})
add_library(VulkanLab::AssetRuntime ALIAS vkl_asset_runtime)
target_include_directories(vkl_asset_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_asset_runtime
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_gpu_runtime
        vkl_glm
        vkl_json
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        vkl_tracy
        ktx_read
)

set(VKL_SCENE_RUNTIME_SOURCES
    scene/GltfModelPrepareFactory.cpp
    scene/GltfPreparer.cpp
    scene/Camera.cpp
    scene/Scene.cpp
    scene/AssetRepository.cpp
    scene/EnvironmentGpuBuilder.cpp
    scene/EnvironmentAssetRepository.cpp
    scene/ModelGpuBuilder.cpp
    scene/ModelLight.cpp
    scene/ModelSourceResolver.cpp
    scene/PrimitiveMeshGenerator.cpp
    scene/PrimitiveModelFactory.cpp
    scene/RuntimeWorld.cpp
    scene/TransformMath.cpp
    scene/SceneLoadManager.cpp
    scene/SceneLoadTask.cpp
    scene/SceneRegistryBuilder.cpp
    scene/SceneRuntimeCoordinator.cpp
)

add_library(vkl_scene_runtime STATIC ${VKL_SCENE_RUNTIME_SOURCES})
add_library(VulkanLab::SceneRuntime ALIAS vkl_scene_runtime)
target_include_directories(vkl_scene_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_scene_runtime
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_gpu_runtime
        vkl_renderer_runtime
        vkl_scene_data
        vkl_glm
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        vkl_gltf_parser
        vkl_image_codecs
        vkl_tracy
)

add_library(vkl_scene_workflow STATIC
    workflows/SceneWorkflowController.cpp
)
add_library(VulkanLab::SceneWorkflow ALIAS vkl_scene_workflow)
target_include_directories(vkl_scene_workflow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_scene_workflow
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_scene_runtime
    PRIVATE
        vkl_foundation
        vkl_tracy
)

add_library(vkl_platform_runtime STATIC
    window/InputManager.cpp
    window/Window.cpp
)
add_library(VulkanLab::PlatformRuntime ALIAS vkl_platform_runtime)
target_include_directories(vkl_platform_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_platform_runtime
    PUBLIC
        vkl_build_options
        vkl_glm
        Vulkan::Vulkan
    PRIVATE
        vkl_glfw
)

if(VKL_ENABLE_EDITOR_UI)
    set(VKL_EDITOR_SOURCES
        editor/EditorController.cpp
        editor/EditorDockWorkspace.cpp
        editor/GuiSystem.cpp
        editor/SceneEditorSession.cpp
        editor/SceneViewportController.cpp
        editor/ReflectionProbeCapture.cpp
        editor/EditorTheme.cpp
        editor/EditorWidgets.cpp
        editor/panels/AssetsPanel.cpp
        editor/panels/InspectorPanel.cpp
        editor/panels/OutlinerPanel.cpp
        editor/panels/ScenesPanel.cpp
    )
    if(VKL_ENABLE_ASSET_AUTHORING)
        list(APPEND VKL_EDITOR_SOURCES platform/FileDialogWin32.cpp)
    endif()
    add_library(vkl_editor STATIC ${VKL_EDITOR_SOURCES})
    add_library(VulkanLab::Editor ALIAS vkl_editor)
    target_include_directories(vkl_editor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(vkl_editor
        PUBLIC
            vkl_build_options
            vkl_asset_core
            vkl_asset_runtime
            vkl_capture
            vkl_gpu_runtime
            vkl_platform_runtime
            vkl_renderer_runtime
            vkl_scene_runtime
            vkl_scene_workflow
            vkl_imgui
            vkl_imguizmo
        PRIVATE
            vkl_foundation
            vkl_image_codecs
            vkl_tracy
            ole32
            uuid
    )
endif()

if(VKL_ENABLE_RUNTIME_CONTROL)
    add_library(vkl_runtime_control_adapter STATIC
        control/RuntimeControlAdapter.cpp
    )
    add_library(VulkanLab::RuntimeControlAdapter ALIAS
                vkl_runtime_control_adapter)
    target_include_directories(vkl_runtime_control_adapter
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(vkl_runtime_control_adapter
        PUBLIC
            vkl_build_options
            vkl_asset_core
            vkl_asset_runtime
            vkl_capture
            vkl_control
            vkl_gpu_runtime
            vkl_platform_runtime
            vkl_renderer_runtime
            vkl_scene_runtime
            vkl_scene_workflow
        PRIVATE
            vkl_foundation
            vkl_json
            vkl_tracy
    )
endif()

# Transitional aggregate for existing downstream targets. It owns no source;
# concrete runtime modules above define the actual build boundaries.
add_library(vkl_engine INTERFACE)
add_library(VulkanLab::Engine ALIAS vkl_engine)
target_link_libraries(vkl_engine INTERFACE
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_capture
        vkl_foundation
        vkl_gpu_runtime
        vkl_platform_runtime
        vkl_renderer_runtime
        vkl_scene_runtime
        vkl_scene_workflow
        vkl_shader_catalog
        vkl_scene_data
)
if(VKL_ENABLE_EDITOR_UI)
    target_link_libraries(vkl_engine INTERFACE vkl_editor)
endif()
if(VKL_ENABLE_RUNTIME_CONTROL)
    target_link_libraries(vkl_engine INTERFACE vkl_runtime_control_adapter)
endif()

add_executable(VulkanLab
    main.cpp
    app/Application.cpp
)
target_include_directories(VulkanLab PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(VulkanLab PRIVATE
    vkl_build_options
    vkl_asset_core
    vkl_asset_runtime
    vkl_capture
    vkl_foundation
    vkl_gpu_runtime
    vkl_platform_runtime
    vkl_renderer_runtime
    vkl_scene_runtime
    vkl_scene_workflow
    vkl_shader_catalog
    vkl_tracy
    vkl_glfw
    vkl_json
    Vulkan::Vulkan
)
if(VKL_ENABLE_RUNTIME_CONTROL)
    target_link_libraries(VulkanLab PRIVATE vkl_runtime_control_adapter)
endif()
if(VKL_ENABLE_EDITOR_UI)
    target_link_libraries(VulkanLab PRIVATE vkl_editor)
endif()
add_dependencies(VulkanLab VulkanLabShaders)

if(VKL_ENABLE_CACAO)
    add_custom_command(TARGET VulkanLab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/license.txt"
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO/license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/NOTICES.txt"
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO/NOTICES.txt"
    )
endif()

# Developer builds locate source assets through vulkanlab_project.json. Only
# the locator belongs beside the executable; cooked packages stage their own
# verified resource closure.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
)
