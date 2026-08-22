set(VKL_CAPTURE_SOURCES
    CaptureTypes.cpp
    CaptureTaskQueue.cpp
)
if(VKL_ENABLE_CAPTURE)
    list(APPEND VKL_CAPTURE_SOURCES CaptureService.cpp)
else()
    list(APPEND VKL_CAPTURE_SOURCES CaptureServiceDisabled.cpp)
endif()

add_library(vkl_capture STATIC ${VKL_CAPTURE_SOURCES})
add_library(VulkanLab::Capture ALIAS vkl_capture)
target_include_directories(vkl_capture PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_capture
    PUBLIC
        vkl_build_options
        vkl_gpu_runtime
    PRIVATE
        vkl_asset_core
        vkl_foundation
        vkl_image_codecs
        vkl_tracy
)
