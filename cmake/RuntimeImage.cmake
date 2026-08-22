include_guard(GLOBAL)

function(vkl_validate_runtime_relative_path relative_path)
    if(NOT relative_path OR IS_ABSOLUTE "${relative_path}" OR
       relative_path MATCHES "(^|/)\\.\\.(/|$)" OR
       relative_path MATCHES "\\\\")
        message(FATAL_ERROR
            "Runtime payload path must be a normalized relative path: "
            "${relative_path}")
    endif()
    cmake_path(NORMAL_PATH relative_path OUTPUT_VARIABLE normalized_path)
    if(NOT relative_path STREQUAL normalized_path)
        message(FATAL_ERROR
            "Runtime payload path must be normalized: ${relative_path}")
    endif()
endfunction()

function(vkl_register_runtime_owned_path)
    set(one_value_args OWNER DESTINATION)
    cmake_parse_arguments(PARSE_ARGV 0 VKL_RUNTIME
        "" "${one_value_args}" "")
    if(NOT VKL_RUNTIME_OWNER OR NOT VKL_RUNTIME_DESTINATION)
        message(FATAL_ERROR
            "vkl_register_runtime_owned_path requires OWNER and DESTINATION")
    endif()
    vkl_validate_runtime_relative_path("${VKL_RUNTIME_DESTINATION}")

    get_property(known_paths GLOBAL
        PROPERTY "VKL_RUNTIME_KNOWN_${VKL_RUNTIME_OWNER}")
    list(APPEND known_paths "${VKL_RUNTIME_DESTINATION}")
    set_property(GLOBAL PROPERTY "VKL_RUNTIME_KNOWN_${VKL_RUNTIME_OWNER}"
        "${known_paths}")
endfunction()

function(vkl_register_runtime_payload)
    set(one_value_args OWNER SOURCE DESTINATION)
    cmake_parse_arguments(PARSE_ARGV 0 VKL_RUNTIME
        "" "${one_value_args}" "")
    foreach(required_arg IN ITEMS OWNER SOURCE DESTINATION)
        if(NOT VKL_RUNTIME_${required_arg})
            message(FATAL_ERROR
                "vkl_register_runtime_payload requires ${required_arg}")
        endif()
    endforeach()

    vkl_register_runtime_owned_path(
        OWNER "${VKL_RUNTIME_OWNER}"
        DESTINATION "${VKL_RUNTIME_DESTINATION}")

    get_property(sources GLOBAL
        PROPERTY "VKL_RUNTIME_SOURCES_${VKL_RUNTIME_OWNER}")
    get_property(destinations GLOBAL
        PROPERTY "VKL_RUNTIME_DESTINATIONS_${VKL_RUNTIME_OWNER}")
    list(APPEND sources "${VKL_RUNTIME_SOURCE}")
    list(APPEND destinations "${VKL_RUNTIME_DESTINATION}")
    set_property(GLOBAL PROPERTY "VKL_RUNTIME_SOURCES_${VKL_RUNTIME_OWNER}"
        "${sources}")
    set_property(GLOBAL
        PROPERTY "VKL_RUNTIME_DESTINATIONS_${VKL_RUNTIME_OWNER}"
        "${destinations}")
endfunction()

function(vkl_define_runtime_payload_target target owner)
    get_property(known_paths GLOBAL PROPERTY "VKL_RUNTIME_KNOWN_${owner}")
    get_property(sources GLOBAL PROPERTY "VKL_RUNTIME_SOURCES_${owner}")
    get_property(destinations GLOBAL
        PROPERTY "VKL_RUNTIME_DESTINATIONS_${owner}")

    list(REMOVE_DUPLICATES known_paths)
    set(unique_destinations)
    set(unique_sources)
    list(LENGTH destinations destination_count)
    if(destination_count GREATER 0)
        math(EXPR destination_last "${destination_count} - 1")
        foreach(index RANGE 0 ${destination_last})
            list(GET destinations ${index} destination)
            list(GET sources ${index} source)
            list(FIND unique_destinations "${destination}" existing_index)
            if(existing_index EQUAL -1)
                list(APPEND unique_destinations "${destination}")
                list(APPEND unique_sources "${source}")
            else()
                list(GET unique_sources ${existing_index} existing_source)
                if(NOT "${source}" STREQUAL "${existing_source}")
                    message(FATAL_ERROR
                        "Runtime payload ${destination} has multiple sources: "
                        "${existing_source} and ${source}")
                endif()
            endif()
        endforeach()
    endif()

    list(SORT known_paths)
    set(sorted_destinations ${unique_destinations})
    list(SORT sorted_destinations)
    set(payload_content "")
    foreach(destination IN LISTS sorted_destinations)
        list(FIND unique_destinations "${destination}" source_index)
        list(GET unique_sources ${source_index} source)
        string(APPEND payload_content "${destination}|${source}\n")
    endforeach()
    set(known_content "")
    foreach(destination IN LISTS known_paths)
        string(APPEND known_content "${destination}\n")
    endforeach()

    set(metadata_root "${PROJECT_BINARY_DIR}/generated/runtime-image")
    file(MAKE_DIRECTORY "${metadata_root}")
    file(GLOB legacy_metadata LIST_DIRECTORIES FALSE
        "${metadata_root}/${owner}-*.expected.txt"
        "${metadata_root}/${owner}-*.cleanup.stamp")
    if(legacy_metadata)
        file(REMOVE ${legacy_metadata})
    endif()
    set(payload_list "${metadata_root}/${owner}-$<CONFIG>.payloads.txt")
    set(known_list "${metadata_root}/${owner}-$<CONFIG>.known.txt")
    set(owned_list "${metadata_root}/${owner}-$<CONFIG>.owned.txt")
    file(GENERATE OUTPUT "${payload_list}" CONTENT "${payload_content}")
    file(GENERATE OUTPUT "${known_list}" CONTENT "${known_content}")

    set(cleanup_target "${target}Cleanup")
    add_custom_target(${cleanup_target}
        COMMAND "${CMAKE_COMMAND}"
            "-DRUNTIME_ROOT=${VKL_RUNTIME_OUTPUT_DIRECTORY}"
            "-DPAYLOAD_LIST=${payload_list}"
            "-DKNOWN_LIST=${known_list}"
            "-DOWNED_LIST=${owned_list}"
            -DCLEAN_ONLY=ON
            -P "${PROJECT_SOURCE_DIR}/cmake/StageRuntimePayloads.cmake"
        DEPENDS
            "${payload_list}"
            "${known_list}"
            "${PROJECT_SOURCE_DIR}/cmake/StageRuntimePayloads.cmake"
        COMMENT "Reconciling ${owner} runtime payload ownership"
        VERBATIM
    )
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}"
            "-DRUNTIME_ROOT=${VKL_RUNTIME_OUTPUT_DIRECTORY}"
            "-DPAYLOAD_LIST=${payload_list}"
            "-DKNOWN_LIST=${known_list}"
            "-DOWNED_LIST=${owned_list}"
            -P "${PROJECT_SOURCE_DIR}/cmake/StageRuntimePayloads.cmake"
        DEPENDS
            ${unique_sources}
            "${payload_list}"
            "${known_list}"
            "${PROJECT_SOURCE_DIR}/cmake/StageRuntimePayloads.cmake"
        COMMENT "Reconciling ${owner} developer runtime payloads"
        VERBATIM
    )
endfunction()
