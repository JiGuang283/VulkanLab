foreach(required_variable IN ITEMS
        RUNTIME_ROOT PAYLOAD_LIST KNOWN_LIST OWNED_LIST)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR
            "StageRuntimePayloads.cmake requires ${required_variable}")
    endif()
endforeach()

cmake_path(NORMAL_PATH RUNTIME_ROOT OUTPUT_VARIABLE normalized_root)
cmake_path(ABSOLUTE_PATH normalized_root NORMALIZE
    OUTPUT_VARIABLE normalized_root)
file(MAKE_DIRECTORY "${normalized_root}")

function(vkl_validate_runtime_relative_path relative_path)
    if(NOT relative_path OR IS_ABSOLUTE "${relative_path}" OR
       relative_path MATCHES "(^|/)\\.\\.(/|$)" OR
       relative_path MATCHES "\\\\")
        message(FATAL_ERROR
            "Runtime ownership contains an unsafe path: ${relative_path}")
    endif()
    cmake_path(NORMAL_PATH relative_path OUTPUT_VARIABLE normalized_path)
    if(NOT relative_path STREQUAL normalized_path)
        message(FATAL_ERROR
            "Runtime ownership path is not normalized: ${relative_path}")
    endif()
endfunction()

function(vkl_read_runtime_paths input_file output_variable)
    set(result)
    if(EXISTS "${input_file}")
        file(STRINGS "${input_file}" entries ENCODING UTF-8)
        foreach(entry IN LISTS entries)
            if(NOT entry)
                continue()
            endif()
            vkl_validate_runtime_relative_path("${entry}")
            list(APPEND result "${entry}")
        endforeach()
    endif()
    list(REMOVE_DUPLICATES result)
    set(${output_variable} "${result}" PARENT_SCOPE)
endfunction()

# Validate every desired source before changing the previous runnable image.
set(expected_paths)
set(payload_sources)
if(EXISTS "${PAYLOAD_LIST}")
    file(STRINGS "${PAYLOAD_LIST}" payload_entries ENCODING UTF-8)
    foreach(payload_entry IN LISTS payload_entries)
        if(NOT payload_entry)
            continue()
        endif()
        string(FIND "${payload_entry}" "|" separator)
        if(separator LESS 1)
            message(FATAL_ERROR
                "Malformed runtime payload entry: ${payload_entry}")
        endif()
        string(SUBSTRING "${payload_entry}" 0 ${separator} destination)
        math(EXPR source_offset "${separator} + 1")
        string(SUBSTRING "${payload_entry}" ${source_offset} -1 source)
        vkl_validate_runtime_relative_path("${destination}")
        if(NOT source OR NOT EXISTS "${source}")
            message(FATAL_ERROR
                "Runtime payload source is missing: ${source}")
        endif()
        list(APPEND expected_paths "${destination}")
        list(APPEND payload_sources "${source}")
    endforeach()
endif()

vkl_read_runtime_paths("${KNOWN_LIST}" known_paths)
vkl_read_runtime_paths("${OWNED_LIST}" previously_owned_paths)
set(managed_paths ${known_paths} ${previously_owned_paths})
list(REMOVE_DUPLICATES managed_paths)

foreach(relative_path IN LISTS managed_paths)
    list(FIND expected_paths "${relative_path}" expected_index)
    if(NOT expected_index EQUAL -1)
        continue()
    endif()

    cmake_path(APPEND normalized_root "${relative_path}"
        OUTPUT_VARIABLE stale_path)
    cmake_path(NORMAL_PATH stale_path OUTPUT_VARIABLE stale_path)
    string(FIND "${stale_path}" "${normalized_root}/" root_prefix)
    if(NOT root_prefix EQUAL 0)
        message(FATAL_ERROR
            "Refusing to remove payload outside runtime root: ${stale_path}")
    endif()
    if(EXISTS "${stale_path}" AND NOT IS_DIRECTORY "${stale_path}")
        file(REMOVE "${stale_path}")
    endif()

    get_filename_component(parent_directory "${stale_path}" DIRECTORY)
    while(NOT parent_directory STREQUAL normalized_root)
        file(GLOB remaining_entries LIST_DIRECTORIES TRUE
            "${parent_directory}/*")
        if(remaining_entries)
            break()
        endif()
        file(REMOVE_RECURSE "${parent_directory}")
        get_filename_component(parent_directory
            "${parent_directory}" DIRECTORY)
    endwhile()
endforeach()

if(NOT CLEAN_ONLY)
    list(LENGTH expected_paths payload_count)
    if(payload_count GREATER 0)
        math(EXPR payload_last "${payload_count} - 1")
        foreach(index RANGE 0 ${payload_last})
            list(GET expected_paths ${index} relative_path)
            list(GET payload_sources ${index} source)
            cmake_path(APPEND normalized_root "${relative_path}"
                OUTPUT_VARIABLE destination)
            get_filename_component(destination_directory
                "${destination}" DIRECTORY)
            file(MAKE_DIRECTORY "${destination_directory}")
            file(COPY_FILE "${source}" "${destination}"
                ONLY_IF_DIFFERENT)
        endforeach()
    endif()
endif()

get_filename_component(owned_directory "${OWNED_LIST}" DIRECTORY)
file(MAKE_DIRECTORY "${owned_directory}")
set(owned_content "")
foreach(relative_path IN LISTS expected_paths)
    string(APPEND owned_content "${relative_path}\n")
endforeach()
set(owned_temporary "${OWNED_LIST}.tmp")
file(WRITE "${owned_temporary}" "${owned_content}")
file(COPY_FILE "${owned_temporary}" "${OWNED_LIST}" ONLY_IF_DIFFERENT)
file(REMOVE "${owned_temporary}")
