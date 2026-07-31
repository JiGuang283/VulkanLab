include_guard(GLOBAL)

# KTX 4.4.2 requires KTX1 to configure its bundled `ktx` CLI, although the
# renderer only consumes KTX2 through ktx_read.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX documentation" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX Java bindings" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX Python bindings" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX OpenGL upload" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX load tests" FORCE)
set(KTX_FEATURE_KTX1 ${VKL_BUILD_ASSET_TOOL}
    CACHE BOOL "Required by KTX 4.4.2 tools" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "Enable KTX2" FORCE)
set(KTX_FEATURE_TOOLS ${VKL_BUILD_ASSET_TOOL}
    CACHE BOOL "Build KTX command-line tools" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/external/ktx" external/ktx
                 EXCLUDE_FROM_ALL)

if(VKL_BUILD_ASSET_TOOL)
    # DirectXTex is an offline-only dependency used to produce native BC7
    # payloads. Runtime builds intentionally keep libktx_read as their only
    # derived-texture dependency.
    set(BUILD_TOOLS OFF CACHE BOOL "Disable DirectXTex tools" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "Disable DirectXTex samples" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "Disable DirectXTex D3D11 helpers" FORCE)
    set(BUILD_DX12 OFF CACHE BOOL "Disable DirectXTex D3D12 helpers" FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL
        "Texture-level scheduling owns BC7 parallelism" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/external/DirectXTex"
                     external/DirectXTex EXCLUDE_FROM_ALL)
endif()

# KTX 4.4.2's read-only target still references the KTX1 constructor from
# its format dispatcher when KTX1 is disabled. Supply the documented
# unsupported-feature result so runtime-only builds can retain KTX2 without
# compiling the KTX1 implementation.
if(NOT KTX_FEATURE_KTX1)
    target_sources(ktx_read PRIVATE
        "${PROJECT_SOURCE_DIR}/src/third_party/Ktx1DisabledStub.c"
    )
    target_include_directories(ktx_read PRIVATE
        "${PROJECT_SOURCE_DIR}/external/ktx/lib"
    )
endif()

# KTX registers CLI tests whenever its tools are enabled, including tools this
# project intentionally does not build. Keep the repository-wide CTest suite
# focused on VulkanLab; KTX is pinned and tested upstream.
get_property(KTX_REGISTERED_TESTS
    DIRECTORY "${PROJECT_SOURCE_DIR}/external/ktx/tests"
    PROPERTY TESTS)
if(KTX_REGISTERED_TESTS)
    set(KTX_CTEST_IGNORE_FILE "${PROJECT_BINARY_DIR}/CTestCustom.cmake")
    file(WRITE "${KTX_CTEST_IGNORE_FILE}" "set(CTEST_CUSTOM_TESTS_IGNORE\n")
    foreach(KTX_REGISTERED_TEST IN LISTS KTX_REGISTERED_TESTS)
        file(APPEND "${KTX_CTEST_IGNORE_FILE}"
            "  \"${KTX_REGISTERED_TEST}\"\n")
    endforeach()
    file(APPEND "${KTX_CTEST_IGNORE_FILE}" ")\n")
endif()

# astc-encoder defaults to the static MSVC runtime even when embedded in an
# /MD application. Align whichever ISA target KTX selected to avoid mixing
# CRT heaps in VulkanLab and the asset tool.
if(MSVC)
    foreach(ASTC_TARGET
            astcenc-avx2-static
            astcenc-sse4.1-static
            astcenc-sse2-static
            astcenc-neon-static
            astcenc-native-static
            astcenc-none-static)
        if(TARGET ${ASTC_TARGET})
            set_property(TARGET ${ASTC_TARGET} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endforeach()
endif()

find_package(Vulkan REQUIRED)

if(BUILD_TESTING)
    add_library(vkl_spirv_reflect STATIC
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect/spirv_reflect.c"
    )
    add_library(VulkanLab::SpirvReflect ALIAS vkl_spirv_reflect)
    target_compile_features(vkl_spirv_reflect PRIVATE c_std_99)
    target_compile_definitions(vkl_spirv_reflect
        PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
    )
    target_include_directories(vkl_spirv_reflect SYSTEM PUBLIC
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect"
        "${PROJECT_SOURCE_DIR}/external/spirv-reflect/include"
    )
    target_link_libraries(vkl_spirv_reflect PUBLIC Vulkan::Vulkan)
endif()

find_path(GLM_INCLUDE_DIR glm/glm.hpp
    HINTS "$ENV{VULKAN_SDK}/Include"
    REQUIRED
)

add_library(vkl_glm INTERFACE)
add_library(VulkanLab::glm ALIAS vkl_glm)
target_include_directories(vkl_glm SYSTEM INTERFACE "${GLM_INCLUDE_DIR}")

add_library(vkl_json INTERFACE)
add_library(VulkanLab::json ALIAS vkl_json)
target_include_directories(vkl_json SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/gltf")

add_library(vkl_spdlog INTERFACE)
add_library(VulkanLab::spdlog ALIAS vkl_spdlog)
target_include_directories(vkl_spdlog SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external")

add_library(vkl_stb_headers INTERFACE)
add_library(VulkanLab::stb_headers ALIAS vkl_stb_headers)
target_include_directories(vkl_stb_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/stb")

add_library(vkl_tinygltf_headers INTERFACE)
add_library(VulkanLab::tinygltf_headers ALIAS vkl_tinygltf_headers)
target_include_directories(vkl_tinygltf_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/gltf")

add_library(vkl_tinyobj_headers INTERFACE)
add_library(VulkanLab::tinyobj_headers ALIAS vkl_tinyobj_headers)
target_include_directories(vkl_tinyobj_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/tiny_obj_loader.h")

add_library(vkl_vma_headers INTERFACE)
add_library(VulkanLab::vma_headers ALIAS vkl_vma_headers)
target_include_directories(vkl_vma_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/vma")

if(NOT WIN32)
    message(FATAL_ERROR "VulkanLab currently requires the Windows GLFW package")
endif()

add_library(vkl_glfw STATIC IMPORTED GLOBAL)
add_library(VulkanLab::glfw ALIAS vkl_glfw)
set_target_properties(vkl_glfw PROPERTIES
    IMPORTED_LOCATION
        "${PROJECT_SOURCE_DIR}/external/glfw/lib-vc2022/glfw3.lib"
    INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/external/glfw/include"
    INTERFACE_LINK_LIBRARIES "user32;gdi32;shell32"
)

add_library(vkl_image_codecs STATIC
    "${PROJECT_SOURCE_DIR}/src/stb_image.cpp"
    "${PROJECT_SOURCE_DIR}/src/stb_image_write.cpp"
)
add_library(VulkanLab::ImageCodecs ALIAS vkl_image_codecs)
target_compile_features(vkl_image_codecs PRIVATE cxx_std_17)
target_link_libraries(vkl_image_codecs PUBLIC vkl_stb_headers)

add_library(vkl_gltf_parser STATIC
    "${PROJECT_SOURCE_DIR}/src/tiny_gltf.cpp"
)
add_library(VulkanLab::GltfParser ALIAS vkl_gltf_parser)
target_compile_features(vkl_gltf_parser PRIVATE cxx_std_17)
target_compile_options(vkl_gltf_parser PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /FS>
)
target_link_libraries(vkl_gltf_parser PUBLIC vkl_tinygltf_headers)

add_library(vkl_obj_parser STATIC
    "${PROJECT_SOURCE_DIR}/src/tiny_obj_loader.cpp"
)
add_library(VulkanLab::ObjParser ALIAS vkl_obj_parser)
target_compile_features(vkl_obj_parser PRIVATE cxx_std_17)
target_link_libraries(vkl_obj_parser PUBLIC vkl_tinyobj_headers)

add_library(vkl_vma_impl STATIC
    "${PROJECT_SOURCE_DIR}/src/vk_mem_alloc.cpp"
)
add_library(VulkanLab::VmaImpl ALIAS vkl_vma_impl)
target_compile_features(vkl_vma_impl PRIVATE cxx_std_17)
target_link_libraries(vkl_vma_impl PUBLIC vkl_vma_headers Vulkan::Vulkan)

if(VKL_ENABLE_EDITOR_UI)
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
endif()
