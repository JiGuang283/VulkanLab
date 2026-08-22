foreach(required_variable IN ITEMS GENERATED_ROOT EXPECTED_LIST)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR
            "CleanShaderGenerated.cmake requires ${required_variable}")
    endif()
endforeach()

file(STRINGS "${EXPECTED_LIST}" expected_outputs ENCODING UTF-8)
file(MAKE_DIRECTORY "${GENERATED_ROOT}")

file(GLOB_RECURSE generated_artifacts LIST_DIRECTORIES FALSE
    "${GENERATED_ROOT}/*.spv"
    "${GENERATED_ROOT}/*.spv.d"
    "${GENERATED_ROOT}/*.spv.tmp")

foreach(artifact IN LISTS generated_artifacts)
    file(RELATIVE_PATH relative_artifact "${GENERATED_ROOT}" "${artifact}")
    cmake_path(CONVERT "${relative_artifact}"
        TO_CMAKE_PATH_LIST relative_artifact)

    set(canonical_output "${relative_artifact}")
    if(canonical_output MATCHES "\\.spv\\.(d|tmp)$")
        string(REGEX REPLACE "\\.(d|tmp)$" ""
            canonical_output "${canonical_output}")
    endif()

    list(FIND expected_outputs "${canonical_output}" expected_index)
    if(expected_index EQUAL -1 OR relative_artifact MATCHES "\\.tmp$")
        file(REMOVE "${artifact}")
    endif()
endforeach()

if(DEFINED STAMP)
    get_filename_component(stamp_directory "${STAMP}" DIRECTORY)
    file(MAKE_DIRECTORY "${stamp_directory}")
    file(TOUCH "${STAMP}")
endif()
