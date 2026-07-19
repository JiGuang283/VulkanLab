cmake_minimum_required(VERSION 3.20)
project(VulkanLab LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VulkanLabProjectLocator.json.in"
    "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
    @ONLY
)

include(CTest)

add_library(vkl_foundation STATIC
    src/core/Log.cpp
)
add_library(VulkanLab::Foundation ALIAS vkl_foundation)
target_include_directories(vkl_foundation PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vkl_foundation PUBLIC
    vkl_build_options
    vkl_spdlog
)

add_library(vkl_shader_catalog INTERFACE)
add_library(VulkanLab::ShaderCatalog ALIAS vkl_shader_catalog)
target_include_directories(vkl_shader_catalog INTERFACE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(vkl_shader_catalog INTERFACE vkl_build_options)

add_library(vkl_asset_core STATIC
    src/assets/ArtifactCachePruner.cpp
    src/assets/ArtifactIndex.cpp
    src/assets/ArtifactStatus.cpp
    src/assets/CacheMutationLock.cpp
    src/assets/ContentHash.cpp
    src/assets/DerivedAssetPaths.cpp
    src/assets/DerivedTextureManifest.cpp
    src/assets/ProjectContext.cpp
    src/assets/RuntimePackage.cpp
    src/assets/SceneCatalog.cpp
    src/assets/SceneCatalogEditor.cpp
    src/assets/SceneImportService.cpp
)
add_library(VulkanLab::AssetCore ALIAS vkl_asset_core)
target_include_directories(vkl_asset_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vkl_asset_core
    PUBLIC
        vkl_build_options
        vkl_glm
    PRIVATE
        vkl_json
        bcrypt
)

add_library(vkl_asset_runtime STATIC
    src/assets/AssetImportManager.cpp
    src/assets/AssetLoadCoordinator.cpp
    src/assets/DerivedTextureCache.cpp
)
add_library(VulkanLab::AssetRuntime ALIAS vkl_asset_runtime)
target_include_directories(vkl_asset_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
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
    src/control/NamedPipeServerWin32.cpp
    src/control/RuntimeCommand.cpp
)
add_library(VulkanLab::Control ALIAS vkl_control)
target_include_directories(vkl_control PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vkl_control
    PUBLIC
        vkl_build_options
        vkl_json
    PRIVATE
        vkl_foundation
)

add_library(vkl_engine STATIC
    src/core/Buffer.cpp
    src/core/DescriptorAllocator.cpp
    src/core/Device.cpp
    src/core/FrameSync.cpp
    src/core/Image.cpp
    src/core/IncrementalUploadQueue.cpp
    src/core/Pipeline.cpp
    src/core/PipelineConfigBuilder.cpp
    src/core/ResourcePoolSelfTest.cpp
    src/core/SwapChain.cpp
    src/core/UploadContext.cpp
    src/core/VulkanContext.cpp
    src/core/VulkanException.cpp
    src/platform/FileDialogWin32.cpp
    src/render/FallbackTextures.cpp
    src/render/GltfPreparer.cpp
    src/render/GuiSystem.cpp
    src/render/MaterialInstance.cpp
    src/render/MaterialTemplate.cpp
    src/render/Mesh.cpp
    src/render/PipelineCache.cpp
    src/render/Renderer.cpp
    src/render/RenderPipeline.cpp
    src/render/RenderQueue.cpp
    src/render/TangentGenerator.cpp
    src/render/Texture.cpp
    src/render/TextureData.cpp
    src/render/pass/MainForwardPass.cpp
    src/scene/BuiltinScenes.cpp
    src/scene/Camera.cpp
    src/scene/Scene.cpp
    src/scene/SceneGpuBuilder.cpp
    src/scene/SceneLoadManager.cpp
    src/scene/SceneLoadTask.cpp
    src/scene/SceneRegistryBuilder.cpp
    src/window/InputManager.cpp
    src/window/Window.cpp
)
add_library(VulkanLab::Engine ALIAS vkl_engine)
target_include_directories(vkl_engine PUBLIC "${PROJECT_SOURCE_DIR}/src")
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

add_library(vkl_asset_tool_core STATIC
    tools/vulkan_lab_asset_tool/CookPackageBuilder.cpp
    tools/vulkan_lab_asset_tool/ProcessRunner.cpp
    tools/vulkan_lab_asset_tool/TextureCacheBuilder.cpp
    tools/vulkan_lab_asset_tool/TextureCachePipeline.cpp
)
add_library(VulkanLab::AssetToolCore ALIAS vkl_asset_tool_core)
target_include_directories(vkl_asset_tool_core PUBLIC
    "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool"
)
target_link_libraries(vkl_asset_tool_core
    PUBLIC
        vkl_build_options
        vkl_asset_core
    PRIVATE
        vkl_shader_catalog
        vkl_glm
        vkl_json
        vkl_image_codecs
        vkl_gltf_parser
        ktx_read
        bcrypt
)

add_executable(VulkanLab
    src/main.cpp
    src/app/Application.cpp
)
target_include_directories(VulkanLab PRIVATE "${PROJECT_SOURCE_DIR}/src")
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

if(WIN32)
    add_executable(VulkanLabCtl
        tools/vulkan_lab_ctl/main.cpp
    )
    target_include_directories(VulkanLabCtl PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(VulkanLabCtl PRIVATE
        vkl_build_options
        vkl_control
    )

    add_executable(VulkanLabAssetTool
        tools/vulkan_lab_asset_tool/main.cpp
    )
    target_link_libraries(VulkanLabAssetTool PRIVATE
        vkl_build_options
        vkl_asset_core
        vkl_asset_tool_core
    )
    if(TARGET ktxtools)
        add_dependencies(VulkanLabAssetTool ktxtools)
        add_custom_command(TARGET VulkanLabAssetTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ktxtools>
                $<TARGET_FILE_DIR:VulkanLabAssetTool>/ktx.exe
        )
    endif()
    add_dependencies(VulkanLab VulkanLabAssetTool)
endif()

if(BUILD_TESTING)
    add_executable(VulkanLabCpuTests
        tests/PreparedSceneDataTests.cpp
        tests/DerivedTextureManifestTests.cpp
        tests/SceneCatalogTests.cpp
        tests/SceneImportServiceTests.cpp
        tests/TextureCachePipelineTests.cpp
        tests/ArtifactStatusTests.cpp
        tests/ArtifactIndexTests.cpp
        tests/ArtifactCachePrunerTests.cpp
        tests/RuntimePackageTests.cpp
        tests/AssetImportManagerTests.cpp
        tests/AssetLoadCoordinatorTests.cpp
        tests/SceneCatalogEditorTests.cpp
    )
    target_link_libraries(VulkanLabCpuTests PRIVATE
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_asset_tool_core
        vkl_engine
    )
    add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)

    if(WIN32)
        add_test(
            NAME VulkanLabAssetToolCatalogImport
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_catalog_test"
                -P "${PROJECT_SOURCE_DIR}/tests/AssetToolCatalogImportTest.cmake"
        )
        add_test(
            NAME VulkanLabAssetToolTextureCache
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_texture_cache_test"
                -P "${PROJECT_SOURCE_DIR}/tests/AssetToolTextureCacheTest.cmake"
        )
        add_test(
            NAME VulkanLabAssetToolCook
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
                "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_cook_test"
                -P "${PROJECT_SOURCE_DIR}/tests/AssetToolCookTest.cmake"
        )
    endif()
endif()

# M2 replaces this legacy PRE_BUILD shader flow with explicit generated outputs.
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(SHADER_SOURCES
    legacy/forward.vert
    legacy/forward.frag
    pbr_lite/forward.vert
    pbr_lite/forward.frag
    pbr_lite/forward_normal_mapped.vert
    pbr_lite/forward_normal_mapped.frag
    material_debug/material.vert
    material_debug/base_color.frag
    material_debug/normal.frag
    material_debug/roughness.frag
    material_debug/metallic.frag
    material_debug/occlusion.frag
    material_debug/emissive.frag
    material_debug/alpha.frag
    material_debug/transmission.frag
)

set(SHADER_COMPILE_COMMANDS)
foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    list(APPEND SHADER_COMPILE_COMMANDS
        COMMAND ${GLSLC}
            "${PROJECT_SOURCE_DIR}/shader/${SHADER_SOURCE}"
            -o "${PROJECT_SOURCE_DIR}/shader/${SHADER_SOURCE}.spv"
    )
endforeach()
add_custom_command(TARGET VulkanLab PRE_BUILD
    ${SHADER_COMPILE_COMMANDS}
    COMMENT "Compiling shader variants..."
)

# M3 replaces these directory copies with a runtime asset manifest.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/shader"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/shader"
        -P "${PROJECT_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/textures"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/textures"
        -P "${PROJECT_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/models"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/models"
        -P "${PROJECT_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake"
)
