include_guard(GLOBAL)

find_package(Vulkan REQUIRED)

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

# These implementation libraries compile vendored single-header sources.
# They deliberately do not inherit VulkanLab's warning or runtime-feature
# interfaces.
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

add_library(vkl_vma_impl STATIC
    "${PROJECT_SOURCE_DIR}/src/vk_mem_alloc.cpp"
)
add_library(VulkanLab::VmaImpl ALIAS vkl_vma_impl)
target_compile_features(vkl_vma_impl PRIVATE cxx_std_17)
target_link_libraries(vkl_vma_impl PUBLIC vkl_vma_headers Vulkan::Vulkan)
