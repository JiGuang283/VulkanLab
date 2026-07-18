cmake_minimum_required(VERSION 3.20)
project(VulkanLab)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

include(CTest)
if(BUILD_TESTING)
    add_executable(VulkanLabCpuTests
        "${CMAKE_SOURCE_DIR}/tests/PreparedSceneDataTests.cpp"
        "${CMAKE_SOURCE_DIR}/src/render/TangentGenerator.cpp"
        "${CMAKE_SOURCE_DIR}/src/render/TextureData.cpp"
    )
    target_include_directories(VulkanLabCpuTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GLM_INCLUDE_DIR}
    )
    target_link_libraries(VulkanLabCpuTests PRIVATE Vulkan::Vulkan)
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
)

# 解决一些 GLFW 静态链接时的 Windows 依赖问题
if(WIN32)
    target_link_libraries(VulkanLab PRIVATE user32 shell32 gdi32)
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

# Copy shaders to build directory
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shader
        $<TARGET_FILE_DIR:VulkanLab>/shader
)

# Copy textures to build directory
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/textures
        $<TARGET_FILE_DIR:VulkanLab>/textures
)

# Copy models to build directory
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/models
        $<TARGET_FILE_DIR:VulkanLab>/models
)
