include_guard(GLOBAL)

add_library(vkl_build_options INTERFACE)
add_library(VulkanLab::BuildOptions ALIAS vkl_build_options)

target_compile_features(vkl_build_options INTERFACE cxx_std_17)
target_compile_definitions(vkl_build_options INTERFACE
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)
target_compile_options(vkl_build_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /FS /W4>
)
