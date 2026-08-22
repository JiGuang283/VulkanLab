add_library(vkl_scene_data STATIC
    PrimitiveModelDefinitions.cpp
    SceneDocument.cpp
    SceneIds.cpp
)
add_library(VulkanLab::SceneData ALIAS vkl_scene_data)
target_include_directories(vkl_scene_data PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_scene_data
    PUBLIC
        vkl_build_options
        vkl_glm
    PRIVATE
        vkl_json
        bcrypt
)
