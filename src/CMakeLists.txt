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
    add_custom_command(TARGET VulkanLab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:VulkanLab>/editor"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/lucide-static/lucide.ttf"
            "$<TARGET_FILE_DIR:VulkanLab>/editor/lucide.ttf"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/Lucide"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/lucide-static/LICENSE"
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/Lucide/LICENSE"
    )
endif()
add_dependencies(VulkanLab VulkanLabShaders)

if(VKL_ENABLE_CACAO)
    add_custom_command(TARGET VulkanLab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/license.txt"
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO/license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO/NOTICES.txt"
            "$<TARGET_FILE_DIR:VulkanLab>/licenses/FidelityFX-CACAO/NOTICES.txt"
    )
endif()

# Developer builds locate source assets through vulkanlab_project.json. Only
# the locator belongs beside the executable; cooked packages stage their own
# verified resource closure.
add_custom_command(TARGET VulkanLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_BINARY_DIR}/generated/vulkanlab_project.json"
        "$<TARGET_FILE_DIR:VulkanLab>/vulkanlab_project.json"
)
