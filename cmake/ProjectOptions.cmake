include_guard(GLOBAL)

add_library(vkl_project_options INTERFACE)
add_library(VulkanLab::ProjectOptions ALIAS vkl_project_options)

target_compile_features(vkl_project_options INTERFACE cxx_std_17)
target_compile_definitions(vkl_project_options INTERFACE
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)
target_compile_options(vkl_project_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /FS>
)

add_library(vkl_project_warnings INTERFACE)
add_library(VulkanLab::ProjectWarnings ALIAS vkl_project_warnings)
target_compile_options(vkl_project_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# BuildFeatures.cmake attaches the generated include directory after project
# feature options have been resolved.
add_library(vkl_runtime_features INTERFACE)
add_library(VulkanLab::RuntimeFeatures ALIAS vkl_runtime_features)

# Runtime modules use all three concerns. Host tools and tests link project
# options/warnings directly so their ABI cannot accidentally depend on a
# generated renderer feature header.
add_library(vkl_build_options INTERFACE)
add_library(VulkanLab::BuildOptions ALIAS vkl_build_options)
target_link_libraries(vkl_build_options INTERFACE
    vkl_project_options
    vkl_project_warnings
    vkl_runtime_features
)
