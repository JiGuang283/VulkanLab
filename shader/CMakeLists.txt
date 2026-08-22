find_program(VKL_SHADER_GLSLC glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(VKL_SHADER_SPIRV_VAL spirv-val
    HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
include("${PROJECT_SOURCE_DIR}/cmake/ShaderCompilation.cmake")

set(VKL_SHADER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(VKL_SHADER_MANIFEST "${VKL_SHADER_SOURCE_ROOT}/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VKL_SHADER_MANIFEST}")

file(READ "${VKL_SHADER_MANIFEST}" VKL_SHADER_MANIFEST_JSON)
string(JSON VKL_SHADER_SCHEMA_VERSION ERROR_VARIABLE VKL_SHADER_JSON_ERROR
    GET "${VKL_SHADER_MANIFEST_JSON}" schemaVersion)
if(VKL_SHADER_JSON_ERROR OR
   (NOT VKL_SHADER_SCHEMA_VERSION EQUAL 2 AND
    NOT VKL_SHADER_SCHEMA_VERSION EQUAL 3))
    message(FATAL_ERROR
        "shader/manifest.json has an unsupported or missing schemaVersion")
endif()
string(JSON VKL_SHADER_PROGRAM_COUNT ERROR_VARIABLE VKL_SHADER_JSON_ERROR
    LENGTH "${VKL_SHADER_MANIFEST_JSON}" programs)
if(VKL_SHADER_JSON_ERROR OR VKL_SHADER_PROGRAM_COUNT LESS 1)
    message(FATAL_ERROR
        "shader/manifest.json must contain at least one program")
endif()

set(VKL_SHADER_JOB_IDS)
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
        set(PROGRAM_OUTPUT_QUALIFIER "")
    elseif(PROGRAM_TARGET_ENV STREQUAL "vulkan1.0" OR
           PROGRAM_TARGET_ENV STREQUAL "vulkan1.2")
        # An explicitly requested environment is part of the binary identity.
        # This permits one source to back programs targeting different Vulkan
        # environments without sharing an output or depfile.
        set(PROGRAM_OUTPUT_QUALIFIER ".${PROGRAM_TARGET_ENV}")
    else()
        message(FATAL_ERROR
            "Unsupported shader targetEnv: ${PROGRAM_TARGET_ENV}")
    endif()

    foreach(STAGE_NAME vertex fragment compute)
        string(JSON SHADER_SOURCE ERROR_VARIABLE STAGE_ERROR
            GET "${VKL_SHADER_MANIFEST_JSON}"
            programs ${PROGRAM_INDEX} ${STAGE_NAME})
        if(STAGE_ERROR)
            continue()
        endif()

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
        if(NOT EXISTS "${VKL_SHADER_SOURCE_ROOT}/${SHADER_SOURCE}")
            message(FATAL_ERROR
                "Shader source from manifest is missing: ${SHADER_SOURCE}")
        endif()

        string(SHA256 SHADER_JOB_HASH
            "${SHADER_SOURCE}|${PROGRAM_TARGET_ENV}|${PROGRAM_OUTPUT_QUALIFIER}")
        set(SHADER_JOB_ID "job_${SHADER_JOB_HASH}")
        list(FIND VKL_SHADER_JOB_IDS "${SHADER_JOB_ID}" SHADER_JOB_INDEX)
        if(SHADER_JOB_INDEX EQUAL -1)
            list(APPEND VKL_SHADER_JOB_IDS "${SHADER_JOB_ID}")
            set("VKL_SHADER_SOURCE_${SHADER_JOB_ID}" "${SHADER_SOURCE}")
            set("VKL_SHADER_ENV_${SHADER_JOB_ID}" "${PROGRAM_TARGET_ENV}")
            set("VKL_SHADER_QUALIFIER_${SHADER_JOB_ID}"
                "${PROGRAM_OUTPUT_QUALIFIER}")
        endif()

        if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_MATERIAL_TEXTURES)
            set("VKL_SHADER_BINDLESS_${SHADER_JOB_ID}" ON)
        endif()
        if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_LIGHTING_MRT)
            set("VKL_SHADER_LIGHTING_MRT_${SHADER_JOB_ID}" ON)
        endif()
        if(STAGE_NAME STREQUAL "fragment" AND PROGRAM_SURFACE_MRT)
            set("VKL_SHADER_SURFACE_MRT_${SHADER_JOB_ID}" ON)
        endif()
    endforeach()
endforeach()

set(VKL_GENERATED_SHADER_ROOT
    "${PROJECT_BINARY_DIR}/generated/$<CONFIG>/shader")
set(VKL_RUNTIME_SHADER_ROOT
    "${VKL_RUNTIME_OUTPUT_DIRECTORY}/shader")
set(VKL_SHADER_EXPECTED_LIST
    "${PROJECT_BINARY_DIR}/generated/shader-expected.txt")

set(VKL_SHADER_RELATIVE_OUTPUTS)
set(VKL_SHADER_GENERATED_OUTPUTS)

function(vkl_register_manifest_shader_variant)
    set(one_value_args SOURCE OUTPUT_SUFFIX TARGET_ENV)
    set(multi_value_args DEFINES)
    cmake_parse_arguments(PARSE_ARGV 0 SHADER_VARIANT
        "" "${one_value_args}" "${multi_value_args}")

    set(relative_output
        "${SHADER_VARIANT_SOURCE}${SHADER_VARIANT_OUTPUT_SUFFIX}")
    set(generated_output
        "${VKL_GENERATED_SHADER_ROOT}/${relative_output}")
    set(runtime_output
        "${VKL_RUNTIME_SHADER_ROOT}/${relative_output}")
    vkl_add_shader_variant(
        SOURCE "${VKL_SHADER_SOURCE_ROOT}/${SHADER_VARIANT_SOURCE}"
        OUTPUT_SUFFIX "${SHADER_VARIANT_OUTPUT_SUFFIX}"
        TARGET_ENV "${SHADER_VARIANT_TARGET_ENV}"
        DEFINES ${SHADER_VARIANT_DEFINES}
        GENERATED_OUTPUT "${generated_output}"
        RUNTIME_OUTPUT "${runtime_output}")

    set(relative_outputs "${VKL_SHADER_RELATIVE_OUTPUTS}")
    list(APPEND relative_outputs "${relative_output}")
    set(VKL_SHADER_RELATIVE_OUTPUTS "${relative_outputs}" PARENT_SCOPE)

    set(generated_outputs "${VKL_SHADER_GENERATED_OUTPUTS}")
    list(APPEND generated_outputs "${generated_output}")
    set(VKL_SHADER_GENERATED_OUTPUTS "${generated_outputs}" PARENT_SCOPE)

endfunction()

foreach(SHADER_JOB_ID IN LISTS VKL_SHADER_JOB_IDS)
    set(SHADER_SOURCE "${VKL_SHADER_SOURCE_${SHADER_JOB_ID}}")
    set(SHADER_TARGET_ENV "${VKL_SHADER_ENV_${SHADER_JOB_ID}}")
    set(SHADER_QUALIFIER "${VKL_SHADER_QUALIFIER_${SHADER_JOB_ID}}")

    vkl_register_manifest_shader_variant(
        SOURCE "${SHADER_SOURCE}"
        OUTPUT_SUFFIX "${SHADER_QUALIFIER}.spv"
        TARGET_ENV "${SHADER_TARGET_ENV}")

    if(DEFINED VKL_SHADER_BINDLESS_${SHADER_JOB_ID})
        vkl_register_manifest_shader_variant(
            SOURCE "${SHADER_SOURCE}"
            OUTPUT_SUFFIX "${SHADER_QUALIFIER}.bindless.spv"
            TARGET_ENV "vulkan1.2"
            DEFINES VKL_BINDLESS_MATERIALS=1)
    endif()

    if(DEFINED VKL_SHADER_LIGHTING_MRT_${SHADER_JOB_ID})
        foreach(MRT_COUNT RANGE 1 2)
            if(MRT_COUNT EQUAL 1)
                set(MRT_DEFINES
                    VKL_FORWARD_SPECULAR_OUTPUT=0
                    VKL_FORWARD_DIFFUSE_OUTPUT=0)
            else()
                set(MRT_DEFINES
                    VKL_FORWARD_SPECULAR_OUTPUT=1
                    VKL_FORWARD_DIFFUSE_OUTPUT=0)
            endif()
            vkl_register_manifest_shader_variant(
                SOURCE "${SHADER_SOURCE}"
                OUTPUT_SUFFIX "${SHADER_QUALIFIER}.mrt${MRT_COUNT}.spv"
                TARGET_ENV "${SHADER_TARGET_ENV}"
                DEFINES ${MRT_DEFINES})
            if(DEFINED VKL_SHADER_BINDLESS_${SHADER_JOB_ID})
                vkl_register_manifest_shader_variant(
                    SOURCE "${SHADER_SOURCE}"
                    OUTPUT_SUFFIX
                        "${SHADER_QUALIFIER}.mrt${MRT_COUNT}.bindless.spv"
                    TARGET_ENV "vulkan1.2"
                    DEFINES VKL_BINDLESS_MATERIALS=1 ${MRT_DEFINES})
            endif()
        endforeach()
    endif()

    if(DEFINED VKL_SHADER_SURFACE_MRT_${SHADER_JOB_ID})
        foreach(MRT_COUNT RANGE 0 2)
            if(MRT_COUNT EQUAL 0)
                set(MRT_DEFINES
                    VKL_SURFACE_NORMAL_OUTPUT=0
                    VKL_SURFACE_MOTION_OUTPUT=0
                    VKL_SURFACE_ALBEDO_OUTPUT=0)
            elseif(MRT_COUNT EQUAL 1)
                set(MRT_DEFINES
                    VKL_SURFACE_NORMAL_OUTPUT=1
                    VKL_SURFACE_MOTION_OUTPUT=0
                    VKL_SURFACE_ALBEDO_OUTPUT=0)
            else()
                set(MRT_DEFINES
                    VKL_SURFACE_NORMAL_OUTPUT=1
                    VKL_SURFACE_MOTION_OUTPUT=1
                    VKL_SURFACE_ALBEDO_OUTPUT=0)
            endif()
            vkl_register_manifest_shader_variant(
                SOURCE "${SHADER_SOURCE}"
                OUTPUT_SUFFIX "${SHADER_QUALIFIER}.mrt${MRT_COUNT}.spv"
                TARGET_ENV "${SHADER_TARGET_ENV}"
                DEFINES ${MRT_DEFINES})
            if(DEFINED VKL_SHADER_BINDLESS_${SHADER_JOB_ID})
                vkl_register_manifest_shader_variant(
                    SOURCE "${SHADER_SOURCE}"
                    OUTPUT_SUFFIX
                        "${SHADER_QUALIFIER}.mrt${MRT_COUNT}.bindless.spv"
                    TARGET_ENV "vulkan1.2"
                    DEFINES VKL_BINDLESS_MATERIALS=1 ${MRT_DEFINES})
            endif()
        endforeach()
    endif()
endforeach()

list(REMOVE_DUPLICATES VKL_SHADER_RELATIVE_OUTPUTS)
list(REMOVE_DUPLICATES VKL_SHADER_GENERATED_OUTPUTS)
list(SORT VKL_SHADER_RELATIVE_OUTPUTS)

set(VKL_SHADER_EXPECTED_CONTENT "")
foreach(relative_output IN LISTS VKL_SHADER_RELATIVE_OUTPUTS)
    string(APPEND VKL_SHADER_EXPECTED_CONTENT "${relative_output}\n")
endforeach()
set(write_expected_list ON)
if(EXISTS "${VKL_SHADER_EXPECTED_LIST}")
    file(READ "${VKL_SHADER_EXPECTED_LIST}" existing_expected_content)
    if(existing_expected_content STREQUAL VKL_SHADER_EXPECTED_CONTENT)
        set(write_expected_list OFF)
    endif()
endif()
if(write_expected_list)
    get_filename_component(expected_list_directory
        "${VKL_SHADER_EXPECTED_LIST}" DIRECTORY)
    file(MAKE_DIRECTORY "${expected_list_directory}")
    file(WRITE "${VKL_SHADER_EXPECTED_LIST}"
        "${VKL_SHADER_EXPECTED_CONTENT}")
endif()

# Manifest edits already trigger CMake reconfiguration. Remove generated
# artifacts that are no longer declared at that point, without introducing a
# timestamp dependency that would rebuild every still-valid shader job.
if(CMAKE_CONFIGURATION_TYPES)
    set(shader_configurations ${CMAKE_CONFIGURATION_TYPES})
else()
    set(shader_configurations "${CMAKE_BUILD_TYPE}")
    if(NOT shader_configurations)
        set(shader_configurations ".")
    endif()
endif()
foreach(shader_configuration IN LISTS shader_configurations)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DGENERATED_ROOT=${PROJECT_BINARY_DIR}/generated/${shader_configuration}/shader"
            "-DEXPECTED_LIST=${VKL_SHADER_EXPECTED_LIST}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CleanShaderGenerated.cmake"
        RESULT_VARIABLE clean_shader_result)
    if(NOT clean_shader_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to clean generated shader outputs for ${shader_configuration}")
    endif()
endforeach()

add_custom_target(VulkanLabShaderCompile
    DEPENDS ${VKL_SHADER_GENERATED_OUTPUTS})

set(VKL_RUNTIME_SHADER_MANIFEST
    "${VKL_RUNTIME_SHADER_ROOT}/manifest.json")
add_custom_target(VulkanLabShaders
    COMMAND "${CMAKE_COMMAND}"
        "-DGENERATED_ROOT=${VKL_GENERATED_SHADER_ROOT}"
        "-DRUNTIME_ROOT=${VKL_RUNTIME_SHADER_ROOT}"
        "-DEXPECTED_LIST=${VKL_SHADER_EXPECTED_LIST}"
        "-DMANIFEST=${VKL_SHADER_MANIFEST}"
        -P "${PROJECT_SOURCE_DIR}/cmake/StageShaderRuntime.cmake"
    DEPENDS
        ${VKL_SHADER_GENERATED_OUTPUTS}
        "${VKL_SHADER_MANIFEST}"
        "${VKL_SHADER_EXPECTED_LIST}"
        "${PROJECT_SOURCE_DIR}/cmake/StageShaderRuntime.cmake"
    COMMENT "Publishing validated shader runtime image"
    VERBATIM
)
add_dependencies(VulkanLabShaders VulkanLabShaderCompile)
