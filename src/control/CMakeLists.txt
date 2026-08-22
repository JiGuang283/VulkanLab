set(VKL_CONTROL_REQUIRED OFF)
if(VKL_ENABLE_RUNTIME_CONTROL OR VKL_BUILD_CONTROL_TOOL OR
   VKL_BUILD_RENDER_TEST OR BUILD_TESTING)
    set(VKL_CONTROL_REQUIRED ON)
endif()

if(VKL_CONTROL_REQUIRED)
    add_library(vkl_control STATIC
        NamedPipeServerWin32.cpp
        RuntimeControlClientWin32.cpp
        RuntimeCommand.cpp
        RuntimeCommandDispatcher.cpp
        RuntimeControlProtocol.cpp
    )
    add_library(VulkanLab::Control ALIAS vkl_control)
    target_include_directories(vkl_control PUBLIC "${VKL_SOURCE_ROOT}")
    target_link_libraries(vkl_control
        PUBLIC
            vkl_build_options
            vkl_json
        PRIVATE
            vkl_foundation
    )
endif()

if(VKL_ENABLE_RUNTIME_CONTROL)
    add_library(vkl_runtime_control_adapter STATIC
        RuntimeControlAdapter.cpp
    )
    add_library(VulkanLab::RuntimeControlAdapter ALIAS
                vkl_runtime_control_adapter)
    target_include_directories(vkl_runtime_control_adapter
        PUBLIC "${VKL_SOURCE_ROOT}")
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
