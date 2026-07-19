if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE OR
   NOT DEFINED BUILD_CONFIG)
    message(FATAL_ERROR "BuildInfo generation arguments are incomplete")
endif()

set(revision "unknown")
set(dirty false)
if(DEFINED GIT_EXECUTABLE AND NOT GIT_EXECUTABLE STREQUAL "" AND
   EXISTS "${GIT_EXECUTABLE}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
                rev-parse --verify HEAD
        RESULT_VARIABLE revision_result
        OUTPUT_VARIABLE revision_output
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(revision_result EQUAL 0 AND NOT revision_output STREQUAL "")
        set(revision "${revision_output}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
                    status --porcelain --untracked-files=no
            RESULT_VARIABLE status_result
            OUTPUT_VARIABLE status_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(status_result EQUAL 0 AND NOT status_output STREQUAL "")
            set(dirty true)
        endif()
    endif()
endif()

set(glslc_version "unknown")
if(DEFINED GLSLC_EXECUTABLE AND NOT GLSLC_EXECUTABLE STREQUAL "" AND
   EXISTS "${GLSLC_EXECUTABLE}")
    execute_process(
        COMMAND "${GLSLC_EXECUTABLE}" --version
        RESULT_VARIABLE glslc_result
        OUTPUT_VARIABLE glslc_output
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(glslc_result EQUAL 0)
        string(REGEX MATCH "^[^\r\n]+" glslc_first_line "${glslc_output}")
        if(NOT glslc_first_line STREQUAL "")
            set(glslc_version "${glslc_first_line}")
        endif()
    endif()
endif()

function(cpp_escape input output)
    set(value "${input}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\r" "" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

cpp_escape("${revision}" revision_cpp)
cpp_escape("${BUILD_CONFIG}" config_cpp)
cpp_escape("${COMPILER_DESCRIPTION}" compiler_cpp)
cpp_escape("${VULKAN_SDK_VERSION}" vulkan_cpp)
cpp_escape("${glslc_version}" glslc_cpp)

set(contents "#pragma once\n\n")
string(APPEND contents "namespace vkr::generated {\n")
string(APPEND contents
    "inline constexpr char kBuildRevision[] = \"${revision_cpp}\";\n")
string(APPEND contents
    "inline constexpr bool kBuildDirty = ${dirty};\n")
string(APPEND contents
    "inline constexpr char kBuildConfiguration[] = \"${config_cpp}\";\n")
string(APPEND contents
    "inline constexpr char kBuildCompiler[] = \"${compiler_cpp}\";\n")
string(APPEND contents
    "inline constexpr char kBuildVulkanSdk[] = \"${vulkan_cpp}\";\n")
string(APPEND contents
    "inline constexpr char kBuildGlslc[] = \"${glslc_cpp}\";\n")
string(APPEND contents "} // namespace vkr::generated\n")

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
set(temporary "${OUTPUT_FILE}.tmp")
file(WRITE "${temporary}" "${contents}")
configure_file("${temporary}" "${OUTPUT_FILE}" COPYONLY)
file(REMOVE "${temporary}")
