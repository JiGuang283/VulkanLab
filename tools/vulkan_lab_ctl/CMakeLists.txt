add_executable(VulkanLabCtl main.cpp)
target_include_directories(VulkanLabCtl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(VulkanLabCtl PRIVATE
    vkl_project_options
    vkl_project_warnings
    vkl_control
)
