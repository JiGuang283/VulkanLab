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
if(VKL_SHADER_JSON_ERROR OR NOT VKL_SHADER_SCHEMA_VERSION EQUAL 2)
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
    string(JSON PROGRAM_MATERIAL_TEXTURES ERROR_VARIABLE MATERIAL_TEXTURES_ERROR
        GET "${VKL_SHADER_MANIFEST_JSON}"
        programs ${PROGRAM_INDEX} materialTextures)
    if(MATERIAL_TEXTURES_ERROR)
        set(PROGRAM_MATERIAL_TEXTURES OFF)
    endif()
    string(JSON PROGRAM_LIGHTING_MRT ERROR_VARIABLE LIGHTING_MRT_ERROR
        GET "${VKL_SHADER_MANIFEST_JSON}"
        programs ${PROGRAM_INDEX} lightingMrt)
    if(LIGHTING_MRT_ERROR)
        set(PROGRAM_LIGHTING_MRT OFF)
    endif()
    string(JSON PROGRAM_SURFACE_MRT ERROR_VARIABLE SURFACE_MRT_ERROR
        GET "${VKL_SHADER_MANIFEST_JSON}"
        programs ${PROGRAM_INDEX} surfaceMrt)
    if(SURFACE_MRT_ERROR)
        set(PROGRAM_SURFACE_MRT OFF)
    endif()
    string(JSON PROGRAM_TARGET_ENV ERROR_VARIABLE TARGET_ENV_ERROR
        GET "${VKL_SHADER_MANIFEST_JSON}"
        programs ${PROGRAM_INDEX} targetEnv)
    if(TARGET_ENV_ERROR)
        set(PROGRAM_TARGET_ENV "vulkan1.0")
    elseif(NOT PROGRAM_TARGET_ENV STREQUAL "vulkan1.0" AND
           NOT PROGRAM_TARGET_ENV STREQUAL "vulkan1.2")
        message(FATAL_ERROR
            "Unsupported shader targetEnv: ${PROGRAM_TARGET_ENV}")
    endif()
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
            string(MAKE_C_IDENTIFIER "${SHADER_SOURCE}" SHADER_SOURCE_ID)
            set(SHADER_TARGET_VARIABLE
                "VKL_SHADER_TARGET_ENV_${SHADER_SOURCE_ID}")
            if(DEFINED ${SHADER_TARGET_VARIABLE} AND
               NOT "${${SHADER_TARGET_VARIABLE}}" STREQUAL
                   "${PROGRAM_TARGET_ENV}")
                message(FATAL_ERROR
                    "Shader source has conflicting target environments: ${SHADER_SOURCE}")
            endif()
            set(${SHADER_TARGET_VARIABLE} "${PROGRAM_TARGET_ENV}")
            if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_MATERIAL_TEXTURES)
                set("VKL_SHADER_BINDLESS_${SHADER_SOURCE_ID}" ON)
            endif()
            if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_LIGHTING_MRT)
                set("VKL_SHADER_LIGHTING_MRT_${SHADER_SOURCE_ID}" ON)
            endif()
            if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_SURFACE_MRT)
                set("VKL_SHADER_SURFACE_MRT_${SHADER_SOURCE_ID}" ON)
            endif()
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
    string(MAKE_C_IDENTIFIER "${SHADER_SOURCE}" SHADER_SOURCE_ID)
    set(SHADER_TARGET_ENV
        "${VKL_SHADER_TARGET_ENV_${SHADER_SOURCE_ID}}")
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
        COMMAND ${GLSLC} --target-env=${SHADER_TARGET_ENV}
            -I "${CMAKE_CURRENT_SOURCE_DIR}"
            "${SHADER_INPUT}" -o "${SHADER_GENERATED_OUTPUT}"
        COMMAND ${SPIRV_VAL} --target-env ${SHADER_TARGET_ENV}
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

    if(DEFINED VKL_SHADER_BINDLESS_${SHADER_SOURCE_ID})
        set(SHADER_BINDLESS_GENERATED_OUTPUT
            "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.bindless.spv")
        set(SHADER_BINDLESS_RUNTIME_OUTPUT
            "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.bindless.spv")
        add_custom_command(
            OUTPUT "${SHADER_BINDLESS_GENERATED_OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
            COMMAND ${GLSLC} --target-env=vulkan1.2
                -DVKL_BINDLESS_MATERIALS=1
                -I "${CMAKE_CURRENT_SOURCE_DIR}"
                "${SHADER_INPUT}" -o "${SHADER_BINDLESS_GENERATED_OUTPUT}"
            COMMAND ${SPIRV_VAL} --target-env vulkan1.2
                "${SHADER_BINDLESS_GENERATED_OUTPUT}"
            DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
            COMMENT "Compiling bindless shader ${SHADER_SOURCE}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${SHADER_BINDLESS_RUNTIME_OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SHADER_BINDLESS_GENERATED_OUTPUT}"
                "${SHADER_BINDLESS_RUNTIME_OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E touch
                "${SHADER_BINDLESS_RUNTIME_OUTPUT}"
            DEPENDS "${SHADER_BINDLESS_GENERATED_OUTPUT}"
            COMMENT "Staging bindless shader ${SHADER_SOURCE}"
            VERBATIM
        )
        list(APPEND VKL_RUNTIME_SHADER_OUTPUTS
            "${SHADER_BINDLESS_RUNTIME_OUTPUT}")
    endif()

    if(DEFINED VKL_SHADER_LIGHTING_MRT_${SHADER_SOURCE_ID})
        foreach(MRT_COUNT RANGE 1 2)
            if(MRT_COUNT EQUAL 1)
                set(MRT_DEFINES
                    -DVKL_FORWARD_SPECULAR_OUTPUT=0
                    -DVKL_FORWARD_DIFFUSE_OUTPUT=0)
            else()
                set(MRT_DEFINES
                    -DVKL_FORWARD_SPECULAR_OUTPUT=1
                    -DVKL_FORWARD_DIFFUSE_OUTPUT=0)
            endif()
            set(SHADER_MRT_GENERATED_OUTPUT
                "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.spv")
            set(SHADER_MRT_RUNTIME_OUTPUT
                "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.spv")
            add_custom_command(
                OUTPUT "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                COMMAND ${GLSLC} --target-env=${SHADER_TARGET_ENV}
                    ${MRT_DEFINES}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    "${SHADER_INPUT}" -o "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMAND ${SPIRV_VAL} --target-env ${SHADER_TARGET_ENV}
                    "${SHADER_MRT_GENERATED_OUTPUT}"
                DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
                COMMENT "Compiling ${MRT_COUNT}-target shader ${SHADER_SOURCE}"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${SHADER_MRT_RUNTIME_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHADER_MRT_GENERATED_OUTPUT}"
                    "${SHADER_MRT_RUNTIME_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E touch
                    "${SHADER_MRT_RUNTIME_OUTPUT}"
                DEPENDS "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMENT "Staging ${MRT_COUNT}-target shader ${SHADER_SOURCE}"
                VERBATIM
            )
            list(APPEND VKL_RUNTIME_SHADER_OUTPUTS
                "${SHADER_MRT_RUNTIME_OUTPUT}")

            if(DEFINED VKL_SHADER_BINDLESS_${SHADER_SOURCE_ID})
                set(SHADER_MRT_BINDLESS_GENERATED_OUTPUT
                    "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.bindless.spv")
                set(SHADER_MRT_BINDLESS_RUNTIME_OUTPUT
                    "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.bindless.spv")
                add_custom_command(
                    OUTPUT "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                    COMMAND ${GLSLC} --target-env=vulkan1.2
                        -DVKL_BINDLESS_MATERIALS=1
                        ${MRT_DEFINES}
                        -I "${CMAKE_CURRENT_SOURCE_DIR}"
                        "${SHADER_INPUT}" -o "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMAND ${SPIRV_VAL} --target-env vulkan1.2
                        "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
                    COMMENT "Compiling bindless ${MRT_COUNT}-target shader ${SHADER_SOURCE}"
                    VERBATIM
                )
                add_custom_command(
                    OUTPUT "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                        "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E touch
                        "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    DEPENDS "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMENT "Staging bindless ${MRT_COUNT}-target shader ${SHADER_SOURCE}"
                    VERBATIM
                )
                list(APPEND VKL_RUNTIME_SHADER_OUTPUTS
                    "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}")
            endif()
        endforeach()
    endif()

    if(DEFINED VKL_SHADER_SURFACE_MRT_${SHADER_SOURCE_ID})
        foreach(MRT_COUNT RANGE 0 2)
            if(MRT_COUNT EQUAL 0)
                set(MRT_DEFINES
                    -DVKL_SURFACE_NORMAL_OUTPUT=0
                    -DVKL_SURFACE_MOTION_OUTPUT=0
                    -DVKL_SURFACE_ALBEDO_OUTPUT=0)
            elseif(MRT_COUNT EQUAL 1)
                set(MRT_DEFINES
                    -DVKL_SURFACE_NORMAL_OUTPUT=1
                    -DVKL_SURFACE_MOTION_OUTPUT=0
                    -DVKL_SURFACE_ALBEDO_OUTPUT=0)
            else()
                set(MRT_DEFINES
                    -DVKL_SURFACE_NORMAL_OUTPUT=1
                    -DVKL_SURFACE_MOTION_OUTPUT=1
                    -DVKL_SURFACE_ALBEDO_OUTPUT=0)
            endif()
            set(SHADER_MRT_GENERATED_OUTPUT
                "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.spv")
            set(SHADER_MRT_RUNTIME_OUTPUT
                "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.spv")
            add_custom_command(
                OUTPUT "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                COMMAND ${GLSLC} --target-env=${SHADER_TARGET_ENV}
                    ${MRT_DEFINES}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    "${SHADER_INPUT}" -o "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMAND ${SPIRV_VAL} --target-env ${SHADER_TARGET_ENV}
                    "${SHADER_MRT_GENERATED_OUTPUT}"
                DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
                COMMENT "Compiling ${MRT_COUNT}-target surface shader ${SHADER_SOURCE}"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${SHADER_MRT_RUNTIME_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHADER_MRT_GENERATED_OUTPUT}"
                    "${SHADER_MRT_RUNTIME_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E touch
                    "${SHADER_MRT_RUNTIME_OUTPUT}"
                DEPENDS "${SHADER_MRT_GENERATED_OUTPUT}"
                COMMENT "Staging ${MRT_COUNT}-target surface shader ${SHADER_SOURCE}"
                VERBATIM
            )
            list(APPEND VKL_RUNTIME_SHADER_OUTPUTS
                "${SHADER_MRT_RUNTIME_OUTPUT}")

            if(DEFINED VKL_SHADER_BINDLESS_${SHADER_SOURCE_ID})
                set(SHADER_MRT_BINDLESS_GENERATED_OUTPUT
                    "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.bindless.spv")
                set(SHADER_MRT_BINDLESS_RUNTIME_OUTPUT
                    "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SOURCE}.mrt${MRT_COUNT}.bindless.spv")
                add_custom_command(
                    OUTPUT "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${VKL_GENERATED_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                    COMMAND ${GLSLC} --target-env=vulkan1.2
                        -DVKL_BINDLESS_MATERIALS=1
                        ${MRT_DEFINES}
                        -I "${CMAKE_CURRENT_SOURCE_DIR}"
                        "${SHADER_INPUT}" -o "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMAND ${SPIRV_VAL} --target-env vulkan1.2
                        "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    DEPENDS "${SHADER_INPUT}" ${VKL_SHADER_INCLUDES}
                    COMMENT "Compiling bindless ${MRT_COUNT}-target surface shader ${SHADER_SOURCE}"
                    VERBATIM
                )
                add_custom_command(
                    OUTPUT "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${VKL_RUNTIME_SHADER_ROOT}/${SHADER_SUBDIRECTORY}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                        "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E touch
                        "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}"
                    DEPENDS "${SHADER_MRT_BINDLESS_GENERATED_OUTPUT}"
                    COMMENT "Staging bindless ${MRT_COUNT}-target surface shader ${SHADER_SOURCE}"
                    VERBATIM
                )
                list(APPEND VKL_RUNTIME_SHADER_OUTPUTS
                    "${SHADER_MRT_BINDLESS_RUNTIME_OUTPUT}")
            endif()
        endforeach()
    endif()
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
