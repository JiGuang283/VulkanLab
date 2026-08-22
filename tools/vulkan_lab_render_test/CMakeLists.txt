add_library(vkl_render_test_core STATIC
    ImageComparator.cpp
    ManagedProcessWin32.cpp
    RenderTestRunner.cpp
    RenderTestSpec.cpp
)
add_library(VulkanLab::RenderTestCore ALIAS vkl_render_test_core)
target_include_directories(vkl_render_test_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(vkl_render_test_core
    PUBLIC
        vkl_project_options
        vkl_project_warnings
        vkl_control
        vkl_json
    PRIVATE
        vkl_asset_core
        vkl_image_codecs
        bcrypt
)

add_executable(VulkanLabRenderTest main.cpp)
target_link_libraries(VulkanLabRenderTest PRIVATE
    vkl_project_options
    vkl_project_warnings
    vkl_render_test_core
)
