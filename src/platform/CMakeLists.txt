add_library(vkl_platform_runtime STATIC
    "${VKL_SOURCE_ROOT}/window/InputManager.cpp"
    "${VKL_SOURCE_ROOT}/window/Window.cpp"
)
add_library(VulkanLab::PlatformRuntime ALIAS vkl_platform_runtime)
target_include_directories(vkl_platform_runtime PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_platform_runtime
    PUBLIC
        vkl_build_options
        vkl_glm
        Vulkan::Vulkan
    PRIVATE
        vkl_glfw
)
