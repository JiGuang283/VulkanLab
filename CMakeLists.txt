cmake_minimum_required(VERSION 3.20)
project(VulkanLab)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 寻找 Vulkan 包
# Vulkan SDK 安装后会自动配置环境变量，CMake 能直接找到它
find_package(Vulkan REQUIRED)

# 2. 设置 GLFW 路径
# 指向我们刚才建立的 external 目录
set(GLFW_DIR "${CMAKE_SOURCE_DIR}/external/glfw")

# 添加 GLFW 头文件目录
include_directories(${GLFW_DIR}/include)

# 添加 GLFW 库文件目录 (这里假设你使用的是 lib-vc2022，如果是旧版 VS 请调整)
link_directories(${GLFW_DIR}/lib-vc2022)

# 3. 添加可执行文件
add_executable(VulkanLab
    src/main.cpp
    src/app.cpp
    src/vulkan_instance.cpp
    src/vulkan_device.cpp
    src/vulkan_swapchain.cpp
    src/vulkan_pipeline.cpp
    src/vulkan_renderpass.cpp
    src/vulkan_framebuffers.cpp
    src/vulkan_commandpool.cpp
    src/vulkan_drawframe.cpp
)

# 添加 src 目录为头文件搜索路径
target_include_directories(VulkanLab PRIVATE ${CMAKE_SOURCE_DIR}/src)

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

# Copy shaders to build directory
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shader
        $<TARGET_FILE_DIR:VulkanLab>/shader
)