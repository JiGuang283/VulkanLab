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

# 添加 GLFW 头文件目录
include_directories(${GLFW_DIR}/include)
include_directories(${STB_DIR})
include_directories(${TINYOBJ_DIR})

# 添加 GLFW 库文件目录 (这里假设你使用的是 lib-vc2022，如果是旧版 VS 请调整)
link_directories(${GLFW_DIR}/lib-vc2022)

# 3. 添加可执行文件
add_executable(VulkanLab
    src/main.cpp
    src/app.cpp
    src/stb_image.cpp
    src/tiny_obj_loader.cpp
    src/vulkan_instance.cpp
    src/vulkan_device.cpp
    src/vulkan_swapchain.cpp
    src/vulkan_pipeline.cpp
    src/vulkan_renderpass.cpp
    src/vulkan_framebuffers.cpp
    src/vulkan_commandpool.cpp
    src/vulkan_drawframe.cpp
    src/vulkan_vertex.cpp
    src/vulkan_uniform.cpp
    src/vulkan_texture.cpp
    src/vulkan_depth.cpp
)

# 添加 src 目录及 GLM 头文件为搜索路径
target_include_directories(VulkanLab PRIVATE ${CMAKE_SOURCE_DIR}/src ${GLM_INCLUDE_DIR})

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

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Find glslc to compile shaders
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
# Compile shaders before building
add_custom_command(TARGET VulkanLab PRE_BUILD
    COMMAND ${GLSLC} shader.vert -o vert.spv
    COMMAND ${GLSLC} shader.frag -o frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/shader
    COMMENT "Compiling shaders..."
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