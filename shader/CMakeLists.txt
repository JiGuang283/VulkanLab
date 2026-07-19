# M2 replaces this legacy PRE_BUILD flow with explicit generated outputs.
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

function(vkl_attach_legacy_shader_build target)
    set(SHADER_SOURCES
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
    )

    set(SHADER_COMPILE_COMMANDS)
    foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
        list(APPEND SHADER_COMPILE_COMMANDS
            COMMAND ${GLSLC}
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${SHADER_SOURCE}"
                -o "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${SHADER_SOURCE}.spv"
        )
    endforeach()

    add_custom_command(TARGET ${target} PRE_BUILD
        ${SHADER_COMPILE_COMMANDS}
        COMMENT "Compiling shader variants..."
    )
endfunction()
