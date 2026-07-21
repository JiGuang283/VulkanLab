find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

set(VKL_SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/global_frame.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/material_push.glsl"
)

set(VKL_SHADER_SOURCES
    legacy/forward.vert
    legacy/forward.frag
    pbr_lite/forward.vert
    pbr_lite/forward.frag
    pbr_lite/forward_normal_mapped.vert
    pbr_lite/forward_normal_mapped.frag
    material_debug/material.vert
    material_debug/base_color.frag
    material_debug/normal.frag
    material_debug/roughness.frag
    material_debug/metallic.frag
    material_debug/occlusion.frag
    material_debug/emissive.frag
    material_debug/alpha.frag
    material_debug/transmission.frag
    material_debug/shadow.frag
    shadow/depth.vert
    shadow/depth_mask.frag
    postprocess/fullscreen.vert
    postprocess/tonemap.frag
)

set(VKL_GENERATED_SHADER_ROOT
    "${PROJECT_BINARY_DIR}/generated/$<CONFIG>/shader")
set(VKL_RUNTIME_SHADER_ROOT
    "${PROJECT_BINARY_DIR}/$<CONFIG>/shader")
set(VKL_RUNTIME_SHADER_OUTPUTS)

foreach(SHADER_SOURCE IN LISTS VKL_SHADER_SOURCES)
    get_filename_component(SHADER_SUBDIRECTORY "${SHADER_SOURCE}" DIRECTORY)
    set(SHADER_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SOURCE}")
    set(SHADER_GENERATED_OUTPUT
        "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.spv")
    set(SHADER_RUNTIME_OUTPUT
        "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.spv")

    add_custom_command(
        OUTPUT "${SHADER_GENERATED_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
        COMMAND ${GLSLC} -I "${CMAKE_CURRENT_SOURCE_DIR}"
            "${SHADER_INPUT}" -o "${SHADER_GENERATED_OUTPUT}"
        COMMAND ${SPIRV_VAL} --target-env vulkan1.0
            "${SHADER_GENERATED_OUTPUT}"
        DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
        COMMENT "Compiling shader ${SHADER_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_RUNTIME_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_GENERATED_OUTPUT}"
            "${SHADER_RUNTIME_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E touch "${SHADER_RUNTIME_OUTPUT}"
        DEPENDS "${SHADER_GENERATED_OUTPUT}"
        COMMENT "Staging shader ${SHADER_SOURCE}"
        VERBATIM
    )
    list(APPEND VKL_RUNTIME_SHADER_OUTPUTS "${SHADER_RUNTIME_OUTPUT}")
endforeach()

add_custom_target(VulkanLabShaders DEPENDS ${VKL_RUNTIME_SHADER_OUTPUTS})
