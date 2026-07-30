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
    render/ShaderRegistry.cpp
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

set(VKL_ASSET_CORE_SOURCES
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
        assets/SceneImportService.cpp
    )
endif()

add_library(vkl_asset_core STATIC ${VKL_ASSET_CORE_SOURCES})
add_library(VulkanLab::AssetCore ALIAS vkl_asset_core)
target_include_directories(vkl_asset_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_asset_core
    PUBLIC
        vkl_build_options
        vkl_glm
    PRIVATE
        vkl_json
        bcrypt
)

set(VKL_ASSET_RUNTIME_SOURCES
    assets/AssetLoadCoordinator.cpp
    assets/DerivedEnvironmentCache.cpp
    assets/DerivedTextureCache.cpp
    assets/EnvironmentLoadManager.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING)
    list(APPEND VKL_ASSET_RUNTIME_SOURCES
        assets/AssetImportManager.cpp
    )
else()
    list(APPEND VKL_ASSET_RUNTIME_SOURCES
        assets/AssetImportManagerDisabled.cpp
    )
endif()

add_library(vkl_asset_runtime STATIC ${VKL_ASSET_RUNTIME_SOURCES})
add_library(VulkanLab::AssetRuntime ALIAS vkl_asset_runtime)
target_include_directories(vkl_asset_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_asset_runtime
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_glm
        vkl_json
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        ktx_read
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

set(VKL_ENGINE_SOURCES
    core/Buffer.cpp
    core/ComputePipeline.cpp
    core/DescriptorAllocator.cpp
    core/Device.cpp
    core/FrameSync.cpp
    core/Image.cpp
    core/IncrementalUploadQueue.cpp
    core/Pipeline.cpp
    core/PipelineConfigBuilder.cpp
    core/ResourcePoolSelfTest.cpp
    core/SwapChain.cpp
    core/UploadContext.cpp
    core/ValidationProfile.cpp
    core/VulkanContext.cpp
    core/VulkanException.cpp
    diagnostics/CaptureTypes.cpp
    render/FallbackTextures.cpp
    render/EnvironmentGpuBuilder.cpp
    render/DirectionalShadow.cpp
    render/GltfPreparer.cpp
    render/MaterialInstance.cpp
    render/MaterialTemplate.cpp
    render/Mesh.cpp
    render/PipelineCache.cpp
    render/Renderer.cpp
    render/RenderPipeline.cpp
    render/RenderQueue.cpp
    render/RenderResourceRegistry.cpp
    render/RenderView.cpp
    render/TangentGenerator.cpp
    render/Texture.cpp
    render/TextureData.cpp
    render/pass/MainForwardPass.cpp
    render/pass/DirectionalShadowPass.cpp
    render/pass/BloomPass.cpp
    render/pass/SkyboxPass.cpp
    render/pass/ToneMapPass.cpp
    scene/BuiltinScenes.cpp
    scene/Camera.cpp
    scene/Scene.cpp
    scene/SceneGpuBuilder.cpp
    scene/SceneLoadManager.cpp
    scene/SceneLoadTask.cpp
    scene/SceneRegistryBuilder.cpp
    window/InputManager.cpp
    window/Window.cpp
)

if(VKL_ENABLE_GPU_DEBUG_UTILS)
    list(APPEND VKL_ENGINE_SOURCES core/GpuDebugUtils.cpp)
else()
    list(APPEND VKL_ENGINE_SOURCES core/GpuDebugUtilsDisabled.cpp)
endif()

if(VKL_ENABLE_GPU_PROFILING)
    list(APPEND VKL_ENGINE_SOURCES render/GpuPassProfiler.cpp)
else()
    list(APPEND VKL_ENGINE_SOURCES render/GpuPassProfilerDisabled.cpp)
endif()

if(VKL_ENABLE_EDITOR_UI)
    list(APPEND VKL_ENGINE_SOURCES render/GuiSystem.cpp)
else()
    list(APPEND VKL_ENGINE_SOURCES render/GuiSystemDisabled.cpp)
endif()

if(VKL_ENABLE_CAPTURE)
    list(APPEND VKL_ENGINE_SOURCES
        diagnostics/CaptureService.cpp
        diagnostics/CaptureTaskQueue.cpp
    )
else()
    list(APPEND VKL_ENGINE_SOURCES
        diagnostics/CaptureServiceDisabled.cpp
    )
endif()

if(VKL_ENABLE_ASSET_AUTHORING AND VKL_ENABLE_EDITOR_UI)
    list(APPEND VKL_ENGINE_SOURCES platform/FileDialogWin32.cpp)
endif()

add_library(vkl_engine STATIC ${VKL_ENGINE_SOURCES})
add_library(VulkanLab::Engine ALIAS vkl_engine)
target_include_directories(vkl_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_engine
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_foundation
        vkl_shader_catalog
        vkl_glm
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_image_codecs
        vkl_gltf_parser
        vkl_obj_parser
        vkl_vma_impl
        vkl_glfw
        ktx_read
        ole32
        uuid
)
if(VKL_ENABLE_EDITOR_UI)
    target_link_libraries(vkl_engine PRIVATE vkl_imgui)
endif()

add_executable(VulkanLab
    main.cpp
    app/Application.cpp
)
target_include_directories(VulkanLab PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(VulkanLab PRIVATE
    vkl_build_options
    vkl_engine
    vkl_asset_core
    vkl_asset_runtime
    vkl_foundation
    vkl_shader_catalog
    vkl_glfw
    vkl_json
    Vulkan::Vulkan
)
if(VKL_ENABLE_RUNTIME_CONTROL)
    target_link_libraries(VulkanLab PRIVATE vkl_control)
endif()
if(VKL_ENABLE_EDITOR_UI)
    target_link_libraries(VulkanLab PRIVATE vkl_imgui)
endif()
add_dependencies(VulkanLab VulkanLabShaders)

# Developer builds locate source assets through vulkanlab_project.json. Only
# the locator belongs beside the executable; cooked packages stage their own
# verified resource closure.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
)
