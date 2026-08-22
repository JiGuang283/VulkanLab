include_guard(GLOBAL)

function(vkl_add_shader_variant)
    set(one_value_args
        SOURCE
        OUTPUT_SUFFIX
        TARGET_ENV
        GENERATED_OUTPUT
        RUNTIME_OUTPUT)
    set(multi_value_args DEFINES)
    cmake_parse_arguments(PARSE_ARGV 0 VKL_SHADER
        "" "${one_value_args}" "${multi_value_args}")

    foreach(required_arg IN ITEMS
            SOURCE OUTPUT_SUFFIX TARGET_ENV GENERATED_OUTPUT
            RUNTIME_OUTPUT)
        if(NOT VKL_SHADER_${required_arg})
            message(FATAL_ERROR
                "vkl_add_shader_variant requires ${required_arg}")
        endif()
    endforeach()
    if(NOT DEFINED VKL_SHADER_SOURCE_ROOT OR
       NOT DEFINED VKL_SHADER_GLSLC OR
       NOT DEFINED VKL_SHADER_SPIRV_VAL)
        message(FATAL_ERROR
            "Shader compiler context must define SOURCE_ROOT, GLSLC, and SPIRV_VAL")
    endif()

    get_filename_component(generated_directory
        "${VKL_SHADER_GENERATED_OUTPUT}" DIRECTORY)
    set(depfile "${VKL_SHADER_GENERATED_OUTPUT}.d")
    set(temporary_output "${VKL_SHADER_GENERATED_OUTPUT}.tmp")

    set(define_arguments)
    foreach(shader_define IN LISTS VKL_SHADER_DEFINES)
        list(APPEND define_arguments "-D${shader_define}")
    endforeach()

    file(RELATIVE_PATH display_source
        "${VKL_SHADER_SOURCE_ROOT}" "${VKL_SHADER_SOURCE}")
    cmake_path(CONVERT "${display_source}" TO_CMAKE_PATH_LIST display_source)

    # Compile into a temporary file so a failed compiler or spirv-val run
    # cannot replace the last known-good canonical SPIR-V. -MT makes the
    # glslc depfile describe the declared CMake OUTPUT rather than the temp.
    add_custom_command(
        OUTPUT "${VKL_SHADER_GENERATED_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${generated_directory}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${temporary_output}" "${depfile}"
        COMMAND "${VKL_SHADER_GLSLC}"
            "--target-env=${VKL_SHADER_TARGET_ENV}"
            ${define_arguments}
            -I "${VKL_SHADER_SOURCE_ROOT}"
            -MD -MF "${depfile}" -MT "${VKL_SHADER_GENERATED_OUTPUT}"
            "${VKL_SHADER_SOURCE}" -o "${temporary_output}"
        COMMAND "${VKL_SHADER_SPIRV_VAL}"
            --target-env "${VKL_SHADER_TARGET_ENV}"
            "${temporary_output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${temporary_output}" "${VKL_SHADER_GENERATED_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E touch
            "${VKL_SHADER_GENERATED_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${temporary_output}"
        DEPENDS
            "${VKL_SHADER_SOURCE}"
        DEPFILE "${depfile}"
        COMMENT
            "Compiling shader ${display_source}${VKL_SHADER_OUTPUT_SUFFIX}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endfunction()
