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

# Source ownership is made explicit in the following M1 step. Keep the current
# discovery behavior here while excluding implementation-only dependency units.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/*.h"
)
list(REMOVE_ITEM SOURCES
    "${PROJECT_SOURCE_DIR}/src/stb_image.cpp"
    "${PROJECT_SOURCE_DIR}/src/stb_image_write.cpp"
    "${PROJECT_SOURCE_DIR}/src/tiny_gltf.cpp"
    "${PROJECT_SOURCE_DIR}/src/tiny_obj_loader.cpp"
    "${PROJECT_SOURCE_DIR}/src/vk_mem_alloc.cpp"
)

add_executable(VulkanLab ${SOURCES})
target_include_directories(VulkanLab PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(VulkanLab PRIVATE
    vkl_build_options
    vkl_glm
    vkl_json
    vkl_spdlog
    vkl_image_codecs
    vkl_gltf_parser
    vkl_obj_parser
    vkl_vma_impl
    vkl_imgui
    vkl_glfw
    Vulkan::Vulkan
    ktx_read
    bcrypt
    ole32
    uuid
)

if(BUILD_TESTING)
    add_executable(VulkanLabCpuTests
        "${PROJECT_SOURCE_DIR}/tests/PreparedSceneDataTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/DerivedTextureManifestTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/SceneCatalogTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/SceneImportServiceTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/TextureCachePipelineTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/ArtifactStatusTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/ArtifactIndexTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/ArtifactCachePrunerTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/RuntimePackageTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/AssetImportManagerTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/AssetLoadCoordinatorTests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/SceneCatalogEditorTests.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/DerivedAssetPaths.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ContentHash.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/CacheMutationLock.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactStatus.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactIndex.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactCachePruner.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/RuntimePackage.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/AssetImportManager.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/AssetLoadCoordinator.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/DerivedTextureManifest.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ProjectContext.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/SceneCatalog.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/SceneCatalogEditor.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/SceneImportService.cpp"
        "${PROJECT_SOURCE_DIR}/src/render/TangentGenerator.cpp"
        "${PROJECT_SOURCE_DIR}/src/render/TextureData.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCachePipeline.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/ProcessRunner.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/CookPackageBuilder.cpp"
    )
    target_include_directories(VulkanLabCpuTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool"
    )
    target_link_libraries(VulkanLabCpuTests PRIVATE
        vkl_build_options
        vkl_glm
        vkl_json
        vkl_vma_headers
        Vulkan::Vulkan
        bcrypt
    )
    add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)
endif()

if(WIN32)
    add_executable(VulkanLabCtl
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_ctl/main.cpp"
    )
    target_include_directories(VulkanLabCtl PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(VulkanLabCtl PRIVATE
        vkl_build_options
        vkl_json
    )

    add_executable(VulkanLabAssetTool
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/main.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/CookPackageBuilder.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCacheBuilder.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCachePipeline.cpp"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool/ProcessRunner.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/DerivedAssetPaths.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactIndex.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactCachePruner.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ArtifactStatus.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ContentHash.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/CacheMutationLock.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/RuntimePackage.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/DerivedTextureManifest.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/ProjectContext.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/SceneCatalog.cpp"
        "${PROJECT_SOURCE_DIR}/src/assets/SceneImportService.cpp"
    )
    target_include_directories(VulkanLabAssetTool PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tools/vulkan_lab_asset_tool"
    )
    target_link_libraries(VulkanLabAssetTool PRIVATE
        vkl_build_options
        vkl_glm
        vkl_json
        vkl_image_codecs
        vkl_gltf_parser
        ktx_read
        bcrypt
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

    if(BUILD_TESTING)
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
