add_library(vkl_asset_tool_core STATIC
    CookClosureResolver.cpp
    CookPackageBuilder.cpp
    EnvironmentCacheBuilder.cpp
    GltfValidator.cpp
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

set(VKL_GLTF_VALIDATOR_EXECUTABLE "" CACHE FILEPATH
    "Path to Khronos glTF Validator 2.0.0-dev.3.10")
set(VKL_GLTF_VALIDATOR_DEFAULT_ROOT
    "${PROJECT_SOURCE_DIR}/external/tools/gltf-validator/2.0.0-dev.3.10")
if(NOT VKL_GLTF_VALIDATOR_EXECUTABLE AND
   EXISTS "${VKL_GLTF_VALIDATOR_DEFAULT_ROOT}/gltf_validator.exe")
    set(VKL_GLTF_VALIDATOR_EXECUTABLE
        "${VKL_GLTF_VALIDATOR_DEFAULT_ROOT}/gltf_validator.exe")
endif()

if(VKL_GLTF_VALIDATOR_EXECUTABLE AND
   EXISTS "${VKL_GLTF_VALIDATOR_EXECUTABLE}")
    get_filename_component(VKL_GLTF_VALIDATOR_ROOT
        "${VKL_GLTF_VALIDATOR_EXECUTABLE}" DIRECTORY)
    add_custom_command(TARGET VulkanLabAssetTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VKL_GLTF_VALIDATOR_EXECUTABLE}"
            "$<TARGET_FILE_DIR:VulkanLabAssetTool>/gltf_validator.exe"
    )
    if(EXISTS "${VKL_GLTF_VALIDATOR_ROOT}/LICENSE")
        add_custom_command(TARGET VulkanLabAssetTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VKL_GLTF_VALIDATOR_ROOT}/LICENSE"
                "$<TARGET_FILE_DIR:VulkanLabAssetTool>/gltf_validator.LICENSE"
        )
    endif()
    if(EXISTS "${VKL_GLTF_VALIDATOR_ROOT}/NOTICES")
        add_custom_command(TARGET VulkanLabAssetTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VKL_GLTF_VALIDATOR_ROOT}/NOTICES"
                "$<TARGET_FILE_DIR:VulkanLabAssetTool>/gltf_validator.NOTICES"
        )
    endif()
else()
    message(STATUS
        "glTF Validator is not installed; run "
        "tools/setup/Install-GltfValidator.ps1 to enable import validation")
endif()
