include_guard(GLOBAL)

add_library(vkl_cacao INTERFACE)
add_library(VulkanLab::Cacao ALIAS vkl_cacao)
if(VKL_ENABLE_CACAO)
    set(VKL_CACAO_ROOT "${PROJECT_SOURCE_DIR}/external/FidelityFX-CACAO")
    set(VKL_CACAO_SOURCE_DIR "${VKL_CACAO_ROOT}/ffx-cacao/src")
    set(VKL_CACAO_INCLUDE_DIR "${VKL_CACAO_ROOT}/ffx-cacao/inc")
    if(NOT EXISTS "${VKL_CACAO_SOURCE_DIR}/ffx_cacao_impl.cpp")
        message(FATAL_ERROR
            "FidelityFX CACAO is not initialized. Run: "
            "git submodule update --init external/FidelityFX-CACAO")
    endif()

    find_program(VKL_DXC_EXECUTABLE dxc
        HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    find_program(VKL_SPIRV_VAL_EXECUTABLE spirv-val
        HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

    set(VKL_CACAO_GENERATED_DIR
        "${PROJECT_BINARY_DIR}/generated/cacao")
    set(VKL_CACAO_SHADER_STAMP
        "${VKL_CACAO_GENERATED_DIR}/cacao_shaders.stamp")
    add_custom_command(
        OUTPUT "${VKL_CACAO_SHADER_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VKL_CACAO_GENERATED_DIR}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/cmake/GenerateCacaoShaders.ps1"
            -Dxc "${VKL_DXC_EXECUTABLE}"
            -SpirvVal "${VKL_SPIRV_VAL_EXECUTABLE}"
            -SourceRoot "${VKL_CACAO_SOURCE_DIR}"
            -OutputRoot "${VKL_CACAO_GENERATED_DIR}"
            -Stamp "${VKL_CACAO_SHADER_STAMP}"
        DEPENDS
            "${PROJECT_SOURCE_DIR}/cmake/GenerateCacaoShaders.ps1"
            "${VKL_CACAO_SOURCE_DIR}/build_shaders_spirv.bat"
            "${VKL_CACAO_SOURCE_DIR}/ffx_cacao.hlsl"
            "${VKL_CACAO_SOURCE_DIR}/ffx_cacao_bindings.hlsl"
            "${VKL_CACAO_SOURCE_DIR}/ffx_cacao_defines.h"
        COMMENT "Generating and validating FidelityFX CACAO SPIR-V"
        VERBATIM
    )
    add_custom_target(vkl_cacao_shaders
        DEPENDS "${VKL_CACAO_SHADER_STAMP}")

    add_library(vkl_cacao_impl STATIC
        "${VKL_CACAO_SOURCE_DIR}/ffx_cacao.cpp"
        "${VKL_CACAO_SOURCE_DIR}/ffx_cacao_impl.cpp"
    )
    add_dependencies(vkl_cacao_impl vkl_cacao_shaders)
    target_compile_features(vkl_cacao_impl PRIVATE cxx_std_17)
    target_compile_definitions(vkl_cacao_impl PUBLIC
        FFX_CACAO_ENABLE_VULKAN)
    target_include_directories(vkl_cacao_impl SYSTEM PUBLIC
        "${VKL_CACAO_INCLUDE_DIR}"
        "${VKL_CACAO_SOURCE_DIR}"
        "${VKL_CACAO_GENERATED_DIR}"
    )
    target_link_libraries(vkl_cacao_impl PUBLIC Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(vkl_cacao_impl PRIVATE
            /wd4100 /wd4127 /wd4244 /wd4267)
    endif()
    target_link_libraries(vkl_cacao INTERFACE vkl_cacao_impl)
endif()
