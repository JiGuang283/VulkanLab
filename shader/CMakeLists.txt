find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

file(GLOB_RECURSE VKL_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.glsl"
)

set(VKL_SHADER_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VKL_SHADER_MANIFEST}")
file(READ "${VKL_SHADER_MANIFEST}" VKL_SHADER_MANIFEST_JSON)
string(JSON VKL_SHADER_SCHEMA_VERSION ERROR_VARIABLE VKL_SHADER_JSON_ERROR
    GET "${VKL_SHADER_MANIFEST_JSON}" schemaVersion)
if(VKL_SHADER_JSON_ERROR OR NOT VKL_SHADER_SCHEMA_VERSION EQUAL 1)
    message(FATAL_ERROR
        "shader/manifest.json has an unsupported or missing schemaVersion")
endif()
string(JSON VKL_SHADER_PROGRAM_COUNT ERROR_VARIABLE VKL_SHADER_JSON_ERROR
    LENGTH "${VKL_SHADER_MANIFEST_JSON}" programs)
if(VKL_SHADER_JSON_ERROR OR VKL_SHADER_PROGRAM_COUNT LESS 1)
    message(FATAL_ERROR
        "shader/manifest.json must contain at least one program")
endif()

set(VKL_SHADER_SOURCES)
math(EXPR VKL_SHADER_PROGRAM_LAST "${VKL_SHADER_PROGRAM_COUNT} - 1")
foreach(PROGRAM_INDEX RANGE 0 ${VKL_SHADER_PROGRAM_LAST})
    foreach(STAGE_NAME vertex fragment compute)
        string(JSON SHADER_SOURCE ERROR_VARIABLE STAGE_ERROR
            GET "${VKL_SHADER_MANIFEST_JSON}"
            programs ${PROGRAM_INDEX} ${STAGE_NAME})
        if(NOT STAGE_ERROR)
            if(IS_ABSOLUTE "${SHADER_SOURCE}" OR
               SHADER_SOURCE MATCHES "(^|/)\\.\\.(/|$)")
                message(FATAL_ERROR
                    "Shader source path must stay inside shader/: ${SHADER_SOURCE}")
            endif()
            cmake_path(NORMAL_PATH SHADER_SOURCE
                OUTPUT_VARIABLE NORMALIZED_SHADER_SOURCE)
            if(NOT SHADER_SOURCE STREQUAL NORMALIZED_SHADER_SOURCE OR
               SHADER_SOURCE MATCHES "\\\\")
                message(FATAL_ERROR
                    "Shader source path must be normalized with '/' separators: ${SHADER_SOURCE}")
            endif()
            if(STAGE_NAME STREQUAL "vertex")
                set(EXPECTED_SHADER_EXTENSION ".vert")
            elseif(STAGE_NAME STREQUAL "fragment")
                set(EXPECTED_SHADER_EXTENSION ".frag")
            else()
                set(EXPECTED_SHADER_EXTENSION ".comp")
            endif()
            get_filename_component(SHADER_EXTENSION "${SHADER_SOURCE}" EXT)
            if(NOT SHADER_EXTENSION STREQUAL EXPECTED_SHADER_EXTENSION)
                message(FATAL_ERROR
                    "Shader ${STAGE_NAME} stage must use ${EXPECTED_SHADER_EXTENSION}: ${SHADER_SOURCE}")
            endif()
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SOURCE}")
                message(FATAL_ERROR
                    "Shader source from manifest is missing: ${SHADER_SOURCE}")
            endif()
            list(APPEND VKL_SHADER_SOURCES "${SHADER_SOURCE}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES VKL_SHADER_SOURCES)

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

set(VKL_RUNTIME_SHADER_MANIFEST
    "${VKL_RUNTIME_SHADER_ROOT}/manifest.json")
add_custom_command(
    OUTPUT "${VKL_RUNTIME_SHADER_MANIFEST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${VKL_RUNTIME_SHADER_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VKL_SHADER_MANIFEST}"
        "${VKL_RUNTIME_SHADER_MANIFEST}"
    COMMAND ${CMAKE_COMMAND} -E touch "${VKL_RUNTIME_SHADER_MANIFEST}"
    DEPENDS "${VKL_SHADER_MANIFEST}"
    COMMENT "Staging shader manifest"
    VERBATIM
)
list(APPEND VKL_RUNTIME_SHADER_OUTPUTS "${VKL_RUNTIME_SHADER_MANIFEST}")

add_custom_target(VulkanLabShaders DEPENDS ${VKL_RUNTIME_SHADER_OUTPUTS})
