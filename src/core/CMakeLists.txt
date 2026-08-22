add_library(vkl_foundation STATIC
    Log.cpp
    "${VKL_SOURCE_ROOT}/diagnostics/BuildInfo.cpp"
    "${VKL_SOURCE_ROOT}/diagnostics/DiagnosticsConfig.cpp"
    "${VKL_SOURCE_ROOT}/diagnostics/SubmissionSerialTracker.cpp"
)
add_library(VulkanLab::Foundation ALIAS vkl_foundation)
target_include_directories(vkl_foundation PUBLIC "${VKL_SOURCE_ROOT}")
target_include_directories(vkl_foundation PRIVATE
    "${VKL_BUILD_INFO_INCLUDE_DIR}"
)
add_dependencies(vkl_foundation VulkanLabBuildInfo)
target_link_libraries(vkl_foundation PUBLIC
    vkl_build_options
    vkl_spdlog
)

set(VKL_GPU_RUNTIME_SOURCES
    AccelerationStructure.cpp
    Buffer.cpp
    DescriptorAllocator.cpp
    Device.cpp
    FrameSync.cpp
    GpuBarrier.h
    Image.cpp
    IncrementalUploadQueue.cpp
    SwapChain.cpp
    UploadContext.cpp
    ValidationProfile.cpp
    VulkanContext.cpp
    VulkanException.cpp
)
if(VKL_ENABLE_GPU_DEBUG_UTILS)
    list(APPEND VKL_GPU_RUNTIME_SOURCES GpuDebugUtils.cpp)
else()
    list(APPEND VKL_GPU_RUNTIME_SOURCES GpuDebugUtilsDisabled.cpp)
endif()
if(VKL_ENABLE_TRACY)
    list(APPEND VKL_GPU_RUNTIME_SOURCES
        "${VKL_SOURCE_ROOT}/diagnostics/TracyProfiler.cpp")
else()
    list(APPEND VKL_GPU_RUNTIME_SOURCES
        "${VKL_SOURCE_ROOT}/diagnostics/TracyProfilerDisabled.cpp")
endif()

add_library(vkl_gpu_runtime STATIC ${VKL_GPU_RUNTIME_SOURCES})
add_library(VulkanLab::GpuRuntime ALIAS vkl_gpu_runtime)
target_include_directories(vkl_gpu_runtime PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_gpu_runtime
    PUBLIC
        vkl_build_options
        vkl_foundation
        vkl_glm
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_tracy
        vkl_vma_impl
)
