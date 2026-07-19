add_library(vkl_foundation STATIC
    core/Log.cpp
)
add_library(VulkanLab::Foundation ALIAS vkl_foundation)
target_include_directories(vkl_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkl_foundation PUBLIC
    vkl_build_options
    vkl_spdlog
)

add_library(vkl_shader_catalog INTERFACE)
add_library(VulkanLab::ShaderCatalog ALIAS vkl_shader_catalog)
target_include_directories(vkl_shader_catalog INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(vkl_shader_catalog INTERFACE vkl_build_options)

add_library(vkl_asset_core STATIC
    assets/ArtifactCachePruner.cpp
    assets/ArtifactIndex.cpp
    assets/ArtifactStatus.cpp
    assets/CacheMutationLock.cpp
    assets/ContentHash.cpp
    assets/DerivedAssetPaths.cpp
    assets/DerivedTextureManifest.cpp
    assets/ProjectContext.cpp
    assets/RuntimePackage.cpp
    assets/SceneCatalog.cpp
    assets/SceneCatalogEditor.cpp
    assets/SceneImportService.cpp
)
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

add_library(vkl_asset_runtime STATIC
    assets/AssetImportManager.cpp
    assets/AssetLoadCoordinator.cpp
    assets/DerivedTextureCache.cpp
)
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

add_library(vkl_control STATIC
    control/NamedPipeServerWin32.cpp
    control/RuntimeCommand.cpp
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

add_library(vkl_engine STATIC
    core/Buffer.cpp
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
    core/VulkanContext.cpp
    core/VulkanException.cpp
    platform/FileDialogWin32.cpp
    render/FallbackTextures.cpp
    render/GltfPreparer.cpp
    render/GuiSystem.cpp
    render/MaterialInstance.cpp
    render/MaterialTemplate.cpp
    render/Mesh.cpp
    render/PipelineCache.cpp
    render/Renderer.cpp
    render/RenderPipeline.cpp
    render/RenderQueue.cpp
    render/TangentGenerator.cpp
    render/Texture.cpp
    render/TextureData.cpp
    render/pass/MainForwardPass.cpp
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
        vkl_imgui
        vkl_glfw
        ktx_read
        ole32
        uuid
)

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
    vkl_control
    vkl_foundation
    vkl_shader_catalog
    vkl_imgui
    vkl_glfw
    vkl_json
    Vulkan::Vulkan
)
add_dependencies(VulkanLab VulkanLabShaders)

# M3 replaces these directory copies with a runtime asset manifest.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/textures"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/textures"
        -P "${PROJECT_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/models"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/models"
        -P "${PROJECT_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake"
)
