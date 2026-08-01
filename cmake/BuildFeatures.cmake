include_guard(GLOBAL)

option(VKL_ENABLE_EDITOR_UI "Compile the Dear ImGui editor workspace" ON)
option(VKL_ENABLE_RUNTIME_CONTROL
    "Compile the local named-pipe Runtime Control server" ON)
option(VKL_ENABLE_CAPTURE "Compile asynchronous PNG capture support" ON)
option(VKL_ENABLE_ASSET_AUTHORING
    "Compile source import and derived-asset authoring support" ON)
option(VKL_ENABLE_VALIDATION
    "Compile Vulkan Validation Layer integration" ON)
option(VKL_ENABLE_GPU_DEBUG_UTILS
    "Compile Vulkan object naming and command labels" ON)
option(VKL_ENABLE_GPU_PROFILING
    "Compile per-pass Vulkan timestamp profiling" ON)
option(VKL_ENABLE_TRACY
    "Compile Tracy CPU and Vulkan GPU profiling" OFF)

option(VKL_BUILD_ASSET_TOOL "Build VulkanLabAssetTool" ON)
option(VKL_BUILD_CONTROL_TOOL "Build VulkanLabCtl" ON)
option(VKL_BUILD_RENDER_TEST "Build VulkanLabRenderTest" ON)

if(VKL_BUILD_RENDER_TEST AND
   (NOT VKL_ENABLE_RUNTIME_CONTROL OR NOT VKL_ENABLE_CAPTURE))
    message(FATAL_ERROR
        "VKL_BUILD_RENDER_TEST requires VKL_ENABLE_RUNTIME_CONTROL=ON "
        "and VKL_ENABLE_CAPTURE=ON")
endif()

if(VKL_ENABLE_ASSET_AUTHORING AND NOT VKL_BUILD_ASSET_TOOL)
    message(WARNING
        "Asset authoring is enabled without a local VulkanLabAssetTool. "
        "OnDemand import requires an external --asset-tool path.")
endif()

set(VKL_BUILD_FEATURES_INCLUDE_DIR
    "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VKL_BUILD_FEATURES_INCLUDE_DIR}")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/BuildFeatures.h.in"
    "${VKL_BUILD_FEATURES_INCLUDE_DIR}/BuildFeatures.h"
)
target_include_directories(vkl_build_options INTERFACE
    "${VKL_BUILD_FEATURES_INCLUDE_DIR}"
)

message(STATUS "VulkanLab build features")
foreach(VKL_FEATURE IN ITEMS
        VKL_ENABLE_EDITOR_UI
        VKL_ENABLE_RUNTIME_CONTROL
        VKL_ENABLE_CAPTURE
        VKL_ENABLE_ASSET_AUTHORING
        VKL_ENABLE_VALIDATION
        VKL_ENABLE_GPU_DEBUG_UTILS
        VKL_ENABLE_GPU_PROFILING
        VKL_ENABLE_TRACY
        VKL_BUILD_ASSET_TOOL
        VKL_BUILD_CONTROL_TOOL
        VKL_BUILD_RENDER_TEST
        BUILD_TESTING)
    message(STATUS "  ${VKL_FEATURE}=${${VKL_FEATURE}}")
endforeach()
