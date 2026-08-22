cmake_minimum_required(VERSION 3.22)
project(VulkanLab LANGUAGES C CXX)

# Keep the runnable developer image separate from CMake intermediates and
# static libraries. Each configure preset has its own PROJECT_BINARY_DIR.
set(VKL_RUNTIME_OUTPUT_ROOT "${PROJECT_BINARY_DIR}/run" CACHE PATH
    "Root for runnable VulkanLab developer images")
set(VKL_RUNTIME_OUTPUT_DIRECTORY
    "${VKL_RUNTIME_OUTPUT_ROOT}/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${VKL_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${VKL_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    "${PROJECT_BINARY_DIR}/lib/$<CONFIG>")
set(CMAKE_PDB_OUTPUT_DIRECTORY
    "${PROJECT_BINARY_DIR}/symbols/$<CONFIG>")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY
    "${PROJECT_BINARY_DIR}/symbols/$<CONFIG>")

include(CTest)
include(cmake/ProjectOptions.cmake)
include(cmake/BuildFeatures.cmake)
include(cmake/Dependencies.cmake)
include(cmake/BuildInfo.cmake)

# Some vendored projects override output properties after reading the global
# CMake defaults. Keep their debug symbols in the project symbols tree.
if(TARGET fmt)
    set_target_properties(fmt PROPERTIES
        COMPILE_PDB_OUTPUT_DIRECTORY
            "${PROJECT_BINARY_DIR}/symbols/$<CONFIG>"
    )
endif()

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VulkanLabProjectLocator.json.in"
    "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
    @ONLY
)

add_subdirectory(shader)
add_subdirectory(src)

# RuntimeImage is the complete runnable renderer payload. VulkanLab owns its
# shader and runtime-file staging, while this target gives build workflows a
# stable dependency that is not an executable-to-tool coupling.
add_custom_target(VulkanLabRuntimeImage DEPENDS VulkanLab)

add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(VKL_DEVELOPER_TARGETS VulkanLabRuntimeImage)
if(TARGET VulkanLabAssetTool)
    list(APPEND VKL_DEVELOPER_TARGETS VulkanLabAssetTool)
endif()
if(TARGET VulkanLabCtl)
    list(APPEND VKL_DEVELOPER_TARGETS VulkanLabCtl)
endif()
add_custom_target(VulkanLabDeveloper DEPENDS ${VKL_DEVELOPER_TARGETS})

set(VKL_FULL_TARGETS VulkanLabDeveloper)
foreach(VKL_FULL_TARGET IN ITEMS
        VulkanLabCtl
        VulkanLabRenderTest
        VulkanLabCpuTests)
    if(TARGET ${VKL_FULL_TARGET})
        list(APPEND VKL_FULL_TARGETS ${VKL_FULL_TARGET})
    endif()
endforeach()
add_custom_target(VulkanLabFull DEPENDS ${VKL_FULL_TARGETS})

if(TARGET VulkanLabAssetTool)
    add_custom_target(VulkanLabCookInput
        DEPENDS VulkanLabRuntimeImage VulkanLabAssetTool)
endif()

include(cmake/TargetOrganization.cmake)
