set(VKL_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# Module order follows concrete target dependencies. Each subdirectory owns
# its source list and link boundary; this root only assembles the application.
add_subdirectory(scene_data)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(assets)
add_subdirectory(diagnostics)
add_subdirectory(scene)
add_subdirectory(workflows)
add_subdirectory(control)
if(VKL_ENABLE_EDITOR_UI)
    add_subdirectory(editor)
endif()

add_executable(VulkanLab
    main.cpp
    app/Application.cpp
)
target_include_directories(VulkanLab PRIVATE "${VKL_SOURCE_ROOT}")
target_link_libraries(VulkanLab PRIVATE
    vkl_build_options
    vkl_asset_core
    vkl_asset_runtime
    vkl_capture
    vkl_foundation
    vkl_gpu_runtime
    vkl_platform_runtime
    vkl_renderer_runtime
    vkl_scene_runtime
    vkl_scene_workflow
    vkl_shader_catalog
    vkl_tracy
    vkl_glfw
    vkl_json
    Vulkan::Vulkan
)
if(VKL_ENABLE_RUNTIME_CONTROL)
    target_link_libraries(VulkanLab PRIVATE vkl_runtime_control_adapter)
endif()
if(VKL_ENABLE_EDITOR_UI)
    target_link_libraries(VulkanLab PRIVATE vkl_editor)
endif()
add_dependencies(VulkanLab VulkanLabShaders)

# Developer builds locate source assets through vulkanlab_project.json. Only
# the locator belongs beside the executable; cooked packages stage their own
# verified resource closure.
vkl_register_runtime_payload(
    OWNER runtime
    SOURCE "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
    DESTINATION "vulkanlab_project.json")

foreach(editor_payload IN ITEMS
        editor/lucide.ttf
        licenses/Lucide/LICENSE)
    vkl_register_runtime_owned_path(
        OWNER runtime DESTINATION "${editor_payload}")
endforeach()
if(VKL_ENABLE_EDITOR_UI)
    vkl_register_runtime_payload(
        OWNER runtime
        SOURCE "${PROJECT_SOURCE_DIR}/external/lucide-static/lucide.ttf"
        DESTINATION "editor/lucide.ttf")
    vkl_register_runtime_payload(
        OWNER runtime
        SOURCE "${PROJECT_SOURCE_DIR}/external/lucide-static/LICENSE"
        DESTINATION "licenses/Lucide/LICENSE")
endif()

foreach(cacao_payload IN ITEMS
        licenses/FidelityFX-CACAO/license.txt
        licenses/FidelityFX-CACAO/NOTICES.txt)
    vkl_register_runtime_owned_path(
        OWNER runtime DESTINATION "${cacao_payload}")
endforeach()
if(VKL_ENABLE_CACAO)
    vkl_register_runtime_payload(
        OWNER runtime
        SOURCE "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/license.txt"
        DESTINATION "licenses/FidelityFX-CACAO/license.txt")
    vkl_register_runtime_payload(
        OWNER runtime
        SOURCE "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/NOTICES.txt"
        DESTINATION "licenses/FidelityFX-CACAO/NOTICES.txt")
endif()
