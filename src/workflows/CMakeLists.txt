add_library(vkl_scene_workflow STATIC
    SceneWorkflowController.cpp
)
add_library(VulkanLab::SceneWorkflow ALIAS vkl_scene_workflow)
target_include_directories(vkl_scene_workflow PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_scene_workflow
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_scene_runtime
    PRIVATE
        vkl_foundation
        vkl_tracy
)
