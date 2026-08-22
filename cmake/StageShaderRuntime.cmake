foreach(required_variable IN ITEMS
        GENERATED_ROOT RUNTIME_ROOT EXPECTED_LIST MANIFEST)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR
            "StageShaderRuntime.cmake requires ${required_variable}")
    endif()
endforeach()

file(STRINGS "${EXPECTED_LIST}" expected_outputs ENCODING UTF-8)
file(MAKE_DIRECTORY "${RUNTIME_ROOT}")

# This script is executed only after every canonical SPIR-V dependency has
# built successfully. Copying and manifest publication therefore form one
# build-level commit point instead of racing per-shader staging commands.
foreach(relative_output IN LISTS expected_outputs)
    set(source "${GENERATED_ROOT}/${relative_output}")
    set(destination "${RUNTIME_ROOT}/${relative_output}")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Compiled shader is missing: ${source}")
    endif()
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_directory}")
    file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT)
endforeach()

file(GLOB_RECURSE runtime_spirv LIST_DIRECTORIES FALSE
    "${RUNTIME_ROOT}/*.spv")
foreach(runtime_file IN LISTS runtime_spirv)
    file(RELATIVE_PATH relative_runtime "${RUNTIME_ROOT}" "${runtime_file}")
    cmake_path(CONVERT "${relative_runtime}"
        TO_CMAKE_PATH_LIST relative_runtime)
    list(FIND expected_outputs "${relative_runtime}" expected_index)
    if(expected_index EQUAL -1)
        file(REMOVE "${runtime_file}")
    endif()
endforeach()

file(COPY_FILE "${MANIFEST}" "${RUNTIME_ROOT}/manifest.json"
    ONLY_IF_DIFFERENT)
if(DEFINED STAMP)
    get_filename_component(stamp_directory "${STAMP}" DIRECTORY)
    file(MAKE_DIRECTORY "${stamp_directory}")
    file(TOUCH "${STAMP}")
endif()
