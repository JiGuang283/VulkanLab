cmake_minimum_required(VERSION 3.22)
project(VulkanLab LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
