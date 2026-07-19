cmake_minimum_required(VERSION 3.20)
project(VulkanLab LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep multi-config artifacts stable when targets move into subdirectories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VulkanLabProjectLocator.json.in"
    "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
    @ONLY
)

include(CTest)

add_subdirectory(shader)
add_subdirectory(src)
add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
