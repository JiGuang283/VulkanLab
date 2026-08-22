include_guard(GLOBAL)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

function(vulkanlab_set_target_folder folder)
    foreach(target_name IN LISTS ARGN)
        if(TARGET ${target_name})
            get_target_property(target_is_imported ${target_name} IMPORTED)
            if(NOT target_is_imported)
                set_property(TARGET ${target_name} PROPERTY FOLDER "${folder}")
            endif()
        endif()
    endforeach()
endfunction()

vulkanlab_set_target_folder("VulkanLab/Runtime"
    VulkanLab
    vkl_foundation
    vkl_shader_catalog
    vkl_scene_data
    vkl_asset_core
    vkl_gpu_runtime
    vkl_capture
    vkl_renderer_runtime
    vkl_asset_runtime
    vkl_scene_runtime
    vkl_scene_workflow
    vkl_platform_runtime
    vkl_runtime_control_adapter
)

vulkanlab_set_target_folder("VulkanLab/Editor"
    vkl_editor
)

vulkanlab_set_target_folder("VulkanLab/Tools"
    VulkanLabAssetTool
    VulkanLabCtl
    VulkanLabRenderTest
    vkl_asset_tool_core
    vkl_render_test_core
)

vulkanlab_set_target_folder("VulkanLab/Tests"
    VulkanLabCpuTests
)

vulkanlab_set_target_folder("VulkanLab/Shaders"
    VulkanLabShaderCompile
    VulkanLabShaders
    vkl_cacao_shaders
)

vulkanlab_set_target_folder("VulkanLab/Workflows"
    VulkanLabRuntimeImage
    VulkanLabRuntimePayloads
    VulkanLabRuntimePayloadsCleanup
    VulkanLabAssetToolRuntimePayloads
    VulkanLabAssetToolRuntimePayloadsCleanup
    VulkanLabDeveloper
    VulkanLabFull
    VulkanLabCookInput
)

vulkanlab_set_target_folder("VulkanLab/Build"
    VulkanLabBuildInfo
    vkl_build_options
    vkl_project_options
    vkl_project_warnings
    vkl_runtime_features
)

# These are the dependency targets directly exposed to VulkanLab's build.
# Vendored projects may retain their own nested folder structure.
vulkanlab_set_target_folder("ThirdParty"
    vkl_tracy
    vkl_cacao
    vkl_cacao_impl
    vkl_spirv_reflect
    vkl_glm
    vkl_json
    vkl_spdlog
    vkl_stb_headers
    vkl_tinygltf_headers
    vkl_vma_headers
    vkl_vma_impl
    vkl_image_codecs
    vkl_gltf_parser
    vkl_imgui
    vkl_imguizmo
    vkl_glfw
    ktx
    ktx_read
    ktxtools
    DirectXTex
    fmt
    objUtil
)
