add_library(vkl_asset_tool_core STATIC
    CookPackageBuilder.cpp
    EnvironmentCacheBuilder.cpp
    NativeBc7Encoder.cpp
    ProcessRunner.cpp
    TextureCacheBuilder.cpp
    TextureCachePipeline.cpp
)
add_library(VulkanLab::AssetToolCore ALIAS vkl_asset_tool_core)
target_include_directories(vkl_asset_tool_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(vkl_asset_tool_core
    PUBLIC
        vkl_build_options
        vkl_asset_core
    PRIVATE
        vkl_shader_catalog
        vkl_glm
        vkl_json
        vkl_image_codecs
        vkl_gltf_parser
        ktx
        DirectXTex
        Vulkan::Vulkan
        bcrypt
)

add_executable(VulkanLabAssetTool main.cpp)
add_dependencies(VulkanLabAssetTool VulkanLabShaders)
target_link_libraries(VulkanLabAssetTool PRIVATE
    vkl_build_options
    vkl_asset_core
    vkl_asset_tool_core
    vkl_json
)

if(TARGET ktxtools)
    add_dependencies(VulkanLabAssetTool ktxtools)
    add_custom_command(TARGET VulkanLabAssetTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ktxtools>
            $<TARGET_FILE_DIR:VulkanLabAssetTool>/ktx.exe
    )
endif()
