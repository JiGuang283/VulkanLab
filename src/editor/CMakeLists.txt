if(NOT VKL_ENABLE_EDITOR_UI)
    return()
endif()

set(VKL_EDITOR_SOURCES
    EditorController.cpp
    EditorDockWorkspace.cpp
    GuiSystem.cpp
    SceneEditorSession.cpp
    SceneViewportController.cpp
    ReflectionProbeCapture.cpp
    EditorTheme.cpp
    EditorWidgets.cpp
    EditorPreferences.cpp
    EditorActionRegistry.cpp
    EditorNotificationService.cpp
    panels/AssetsPanel.cpp
    panels/ContentBrowserPanel.cpp
    panels/DiagnosticsPanel.cpp
    panels/MaterialsPanel.cpp
    panels/InspectorPanel.cpp
    panels/OutlinerPanel.cpp
    panels/RenderSettingsPanel.cpp
    panels/ScenesPanel.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING)
    list(APPEND VKL_EDITOR_SOURCES
        "${VKL_SOURCE_ROOT}/platform/FileDialogWin32.cpp")
endif()

add_library(vkl_editor STATIC ${VKL_EDITOR_SOURCES})
add_library(VulkanLab::Editor ALIAS vkl_editor)
target_include_directories(vkl_editor PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_editor
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_capture
        vkl_gpu_runtime
        vkl_platform_runtime
        vkl_renderer_runtime
        vkl_scene_runtime
        vkl_scene_workflow
        vkl_imgui
        vkl_imguizmo
    PRIVATE
        vkl_foundation
        vkl_image_codecs
        vkl_json
        vkl_tracy
        ole32
        uuid
)
