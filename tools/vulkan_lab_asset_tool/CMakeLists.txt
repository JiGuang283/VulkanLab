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
        vkl_project_options
        vkl_project_warnings
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
target_link_libraries(VulkanLabAssetTool PRIVATE
    vkl_project_options
    vkl_project_warnings
    vkl_asset_core
    vkl_asset_tool_core
    vkl_json
)

if(TARGET ktxtools)
    # KTX-Software overrides CMAKE_RUNTIME_OUTPUT_DIRECTORY inside its own
    # project. Put its CLI back into the same runnable image as AssetTool.
    set_target_properties(ktxtools PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${VKL_RUNTIME_OUTPUT_DIRECTORY}"
    )
    add_dependencies(VulkanLabAssetTool ktxtools)
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
