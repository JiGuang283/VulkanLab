include_guard(GLOBAL)

if(NOT VKL_ENABLE_EDITOR_UI)
    return()
endif()

set(VKL_IMGUI_DIR "${PROJECT_SOURCE_DIR}/external/imgui")
add_library(vkl_imgui STATIC
    "${VKL_IMGUI_DIR}/imgui.cpp"
    "${VKL_IMGUI_DIR}/imgui_draw.cpp"
    "${VKL_IMGUI_DIR}/imgui_tables.cpp"
    "${VKL_IMGUI_DIR}/imgui_widgets.cpp"
    "${VKL_IMGUI_DIR}/imgui_demo.cpp"
    "${VKL_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${VKL_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)
add_library(VulkanLab::ImGui ALIAS vkl_imgui)
target_compile_features(vkl_imgui PRIVATE cxx_std_17)
target_include_directories(vkl_imgui SYSTEM PUBLIC
    "${VKL_IMGUI_DIR}"
    "${VKL_IMGUI_DIR}/backends"
)
target_link_libraries(vkl_imgui PUBLIC vkl_glfw Vulkan::Vulkan)

set(VKL_IMGUIZMO_DIR "${PROJECT_SOURCE_DIR}/external/ImGuizmo")
add_library(vkl_imguizmo STATIC
    "${VKL_IMGUIZMO_DIR}/src/ImGuizmo.cpp"
)
add_library(VulkanLab::ImGuizmo ALIAS vkl_imguizmo)
target_compile_features(vkl_imguizmo PRIVATE cxx_std_17)
target_include_directories(vkl_imguizmo SYSTEM PUBLIC
    "${VKL_IMGUIZMO_DIR}/src"
)
target_link_libraries(vkl_imguizmo PUBLIC vkl_imgui)
