include_guard(GLOBAL)

add_library(vkl_tracy INTERFACE)
add_library(VulkanLab::Tracy ALIAS vkl_tracy)
if(VKL_ENABLE_TRACY)
    # Tracy's upstream defaults enable discovery, remote connections and
    # optional collectors. VulkanLab profiling is intentionally on-demand and
    # localhost-only, so pin every relevant cache option before add_subdirectory.
    set(TRACY_ENABLE ON CACHE BOOL "Enable Tracy profiling" FORCE)
    set(TRACY_STATIC ON CACHE BOOL "Build Tracy as a static library" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL
        "Only collect Tracy data while a profiler is connected" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL
        "Restrict Tracy connections to this machine" FORCE)
    set(TRACY_NO_BROADCAST ON CACHE BOOL
        "Disable Tracy network discovery broadcasts" FORCE)
    set(TRACY_ONLY_IPV4 ON CACHE BOOL
        "Use IPv4 for the localhost Tracy connection" FORCE)
    set(TRACY_NO_CALLSTACK ON CACHE BOOL
        "Disable Tracy callstack collection" FORCE)
    set(TRACY_NO_SAMPLING ON CACHE BOOL
        "Disable Tracy callstack sampling" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL
        "Disable Tracy frame image support" FORCE)
    set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL
        "Do not replace VulkanLab crash handling" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/external/tracy" external/tracy
                     EXCLUDE_FROM_ALL)
    target_link_libraries(vkl_tracy INTERFACE Tracy::TracyClient)
endif()

if(BUILD_TESTING)
    add_library(vkl_spirv_reflect STATIC
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect/spirv_reflect.c"
    )
    add_library(VulkanLab::SpirvReflect ALIAS vkl_spirv_reflect)
    target_compile_features(vkl_spirv_reflect PRIVATE c_std_99)
    target_compile_definitions(vkl_spirv_reflect
        PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
    )
    target_include_directories(vkl_spirv_reflect SYSTEM PUBLIC
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect"
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect/include"
    )
    target_link_libraries(vkl_spirv_reflect PUBLIC Vulkan::Vulkan)
endif()
