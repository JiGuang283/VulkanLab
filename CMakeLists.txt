cmake_minimum_required(VERSION 3.20)
project(VulkanLab)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# KTX 4.4.2 requires KTX1 to configure its bundled `ktx` CLI, although the
# renderer only consumes KTX2 through ktx_read.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX documentation" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX Java bindings" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX Python bindings" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX OpenGL upload" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX load tests" FORCE)
set(KTX_FEATURE_KTX1 ON CACHE BOOL "Required by KTX 4.4.2 tools" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "Enable KTX2" FORCE)
set(KTX_FEATURE_TOOLS ON CACHE BOOL "Build KTX command-line tools" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS" FORCE)
add_subdirectory(external/ktx EXCLUDE_FROM_ALL)

# KTX registers CLI tests whenever its tools are enabled, including tools this
# project intentionally does not build. Keep the repository-wide CTest suite
# focused on VulkanLab; KTX is pinned and tested upstream.
get_property(KTX_REGISTERED_TESTS
    DIRECTORY "${CMAKE_SOURCE_DIR}/external/ktx/tests"
    PROPERTY TESTS)
if(KTX_REGISTERED_TESTS)
    set(KTX_CTEST_IGNORE_FILE
        "${CMAKE_BINARY_DIR}/CTestCustom.cmake")
    file(WRITE "${KTX_CTEST_IGNORE_FILE}"
        "set(CTEST_CUSTOM_TESTS_IGNORE\n")
    foreach(KTX_REGISTERED_TEST IN LISTS KTX_REGISTERED_TESTS)
        file(APPEND "${KTX_CTEST_IGNORE_FILE}"
            "  \"${KTX_REGISTERED_TEST}\"\n")
    endforeach()
    file(APPEND "${KTX_CTEST_IGNORE_FILE}" ")\n")
endif()

# astc-encoder defaults to the static MSVC runtime even when embedded in a
# /MD application. Align whichever ISA target KTX selected to avoid mixing
# CRT heaps in VulkanLab and the asset tool.
if(MSVC)
    foreach(ASTC_TARGET
            astcenc-avx2-static
            astcenc-sse4.1-static
            astcenc-sse2-static
            astcenc-neon-static
            astcenc-native-static
            astcenc-none-static)
        if(TARGET ${ASTC_TARGET})
            set_property(TARGET ${ASTC_TARGET} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endforeach()
endif()

# 1. 寻找 Vulkan 包
# Vulkan SDK 安装后会自动配置环境变量，CMake 能直接找到它
find_package(Vulkan REQUIRED)

# 寻找 GLM（随 Vulkan SDK 一同安装，纯头文件库）
find_path(GLM_INCLUDE_DIR glm/glm.hpp
    HINTS $ENV{VULKAN_SDK}/Include
    REQUIRED
)

# 2. 设置 GLFW 路径
# 指向我们刚才建立的 external 目录
set(GLFW_DIR "${CMAKE_SOURCE_DIR}/external/glfw")
set(STB_DIR "${CMAKE_SOURCE_DIR}/external/stb")
set(TINYOBJ_DIR "${CMAKE_SOURCE_DIR}/external/tiny_obj_loader.h")
set(VMA_DIR "${CMAKE_SOURCE_DIR}/external/vma")
set(GLTF_DIR "${CMAKE_SOURCE_DIR}/external/gltf")
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
set(SPDLOG_ROOT "${CMAKE_SOURCE_DIR}/external")

# 添加 GLFW 头文件目录
include_directories(${GLFW_DIR}/include)
include_directories(${STB_DIR})
include_directories(${TINYOBJ_DIR})
include_directories(${VMA_DIR})
include_directories(${GLTF_DIR})
include_directories(${IMGUI_DIR})
include_directories(${IMGUI_DIR}/backends)

# 添加 GLFW 库文件目录 (这里假设你使用的是 lib-vc2022，如果是旧版 VS 请调整)
link_directories(${GLFW_DIR}/lib-vc2022)

# 3. 添加可执行文件
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

add_executable(VulkanLab ${SOURCES} ${IMGUI_SOURCES})

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/VulkanLabProjectLocator.json.in"
    "${CMAKE_BINARY_DIR}/generated/vulkanlab_project.json"
    @ONLY
)

include(CTest)
if(BUILD_TESTING)
    add_executable(VulkanLabCpuTests
        "${CMAKE_SOURCE_DIR}/tests/PreparedSceneDataTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/DerivedTextureManifestTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/SceneCatalogTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/SceneImportServiceTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/TextureCachePipelineTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/ArtifactStatusTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/ArtifactIndexTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/ArtifactCachePrunerTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/RuntimePackageTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/AssetImportManagerTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/AssetLoadCoordinatorTests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/SceneCatalogEditorTests.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/DerivedAssetPaths.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ContentHash.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/CacheMutationLock.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactStatus.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactIndex.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactCachePruner.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/RuntimePackage.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/AssetImportManager.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/AssetLoadCoordinator.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/DerivedTextureManifest.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ProjectContext.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/SceneCatalog.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/SceneCatalogEditor.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/SceneImportService.cpp"
        "${CMAKE_SOURCE_DIR}/src/render/TangentGenerator.cpp"
        "${CMAKE_SOURCE_DIR}/src/render/TextureData.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCachePipeline.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/ProcessRunner.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/CookPackageBuilder.cpp"
    )
    target_include_directories(VulkanLabCpuTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool
        ${GLM_INCLUDE_DIR}
        ${GLTF_DIR}
    )
    target_link_libraries(VulkanLabCpuTests PRIVATE Vulkan::Vulkan bcrypt)
    target_compile_options(VulkanLabCpuTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)
endif()

if(WIN32)
    add_executable(VulkanLabCtl
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_ctl/main.cpp"
    )
    target_include_directories(VulkanLabCtl PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GLTF_DIR}
    )
    target_compile_options(VulkanLabCtl PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )

    add_executable(VulkanLabAssetTool
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/main.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/CookPackageBuilder.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCacheBuilder.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/TextureCachePipeline.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool/ProcessRunner.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/DerivedAssetPaths.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactIndex.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactCachePruner.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ArtifactStatus.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ContentHash.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/CacheMutationLock.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/RuntimePackage.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/DerivedTextureManifest.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/ProjectContext.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/SceneCatalog.cpp"
        "${CMAKE_SOURCE_DIR}/src/assets/SceneImportService.cpp"
        "${CMAKE_SOURCE_DIR}/src/tiny_gltf.cpp"
        "${CMAKE_SOURCE_DIR}/src/stb_image.cpp"
        "${CMAKE_SOURCE_DIR}/src/stb_image_write.cpp"
    )
    target_include_directories(VulkanLabAssetTool PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tools/vulkan_lab_asset_tool
        ${GLM_INCLUDE_DIR}
        ${STB_DIR}
        ${GLTF_DIR}
    )
    target_link_libraries(VulkanLabAssetTool PRIVATE
        ktx_read
        bcrypt
    )
    target_compile_options(VulkanLabAssetTool PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
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
                "-DTEST_ROOT=${CMAKE_BINARY_DIR}/asset_tool_catalog_test"
                -P "${CMAKE_SOURCE_DIR}/tests/AssetToolCatalogImportTest.cmake"
        )
        add_test(
            NAME VulkanLabAssetToolTextureCache
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
                "-DTEST_ROOT=${CMAKE_BINARY_DIR}/asset_tool_texture_cache_test"
                -P "${CMAKE_SOURCE_DIR}/tests/AssetToolTextureCacheTest.cmake"
        )
        add_test(
            NAME VulkanLabAssetToolCook
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
                "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
                "-DTEST_ROOT=${CMAKE_BINARY_DIR}/asset_tool_cook_test"
                -P "${CMAKE_SOURCE_DIR}/tests/AssetToolCookTest.cmake"
        )
    endif()
endif()

# 添加 src 目录及 GLM 头文件为搜索路径
target_include_directories(VulkanLab PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GLM_INCLUDE_DIR}
    ${SPDLOG_ROOT}
)

target_compile_definitions(VulkanLab PRIVATE
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

target_compile_options(VulkanLab PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /FS>
)

# 4. 链接库
# 链接 Vulkan 和 GLFW
# glfw3.lib 是静态库，glfw3dll.lib 是动态库，通常开发建议链接动态库或静态库均可
# 这里链接静态库 glfw3.lib，同时需要链接 opengl32 等系统库
target_link_libraries(VulkanLab PRIVATE
    Vulkan::Vulkan
    glfw3
    ktx_read
    bcrypt
)

# 解决一些 GLFW 静态链接时的 Windows 依赖问题
if(WIN32)
    target_link_libraries(VulkanLab PRIVATE user32 shell32 gdi32 ole32 uuid)
endif()

# Find glslc to compile shaders
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)

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
foreach(SHADER_SOURCE ${SHADER_SOURCES})
    list(APPEND SHADER_COMPILE_COMMANDS
        COMMAND ${GLSLC}
            ${CMAKE_SOURCE_DIR}/shader/${SHADER_SOURCE}
            -o ${CMAKE_SOURCE_DIR}/shader/${SHADER_SOURCE}.spv
    )
endforeach()

# Compile shader variants before building
add_custom_command(TARGET VulkanLab PRE_BUILD
    ${SHADER_COMPILE_COMMANDS}
    COMMENT "Compiling shader variants..."
)

# Copy runtime assets while preserving source timestamps. KTX2 manifests use
# size + mtime for fast validation, so rewriting unchanged assets invalidates
# otherwise valid derived caches.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/shader"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/shader"
        -P ${CMAKE_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/textures"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/textures"
        -P ${CMAKE_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/models"
        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:VulkanLab>/models"
        -P ${CMAKE_SOURCE_DIR}/cmake/CopyDirectoryPreserveTimestamps.cmake
)
