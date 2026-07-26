if(NOT DEFINED RUNTIME_DIR OR
   NOT EXISTS "${RUNTIME_DIR}/VulkanLab.exe")
    message(FATAL_ERROR "VulkanLab runtime directory was not provided")
endif()

foreach(unexpected models textures)
    if(EXISTS "${RUNTIME_DIR}/${unexpected}")
        message(FATAL_ERROR
            "developer runtime unexpectedly contains ${unexpected}/")
    endif()
endforeach()

if(NOT EXISTS "${RUNTIME_DIR}/vulkanlab_project.json")
    message(FATAL_ERROR "developer runtime is missing its project locator")
endif()

set(shader_manifest "${RUNTIME_DIR}/shader/manifest.json")
if(NOT EXISTS "${shader_manifest}")
    message(FATAL_ERROR "developer runtime is missing shader/manifest.json")
endif()
file(READ "${shader_manifest}" shader_manifest_json)
string(JSON program_count LENGTH "${shader_manifest_json}" programs)
math(EXPR program_last "${program_count} - 1")
set(expected_shaders)
foreach(program_index RANGE 0 ${program_last})
    foreach(stage vertex fragment compute)
        string(JSON source ERROR_VARIABLE stage_error
            GET "${shader_manifest_json}"
            programs ${program_index} ${stage})
        if(NOT stage_error)
            list(APPEND expected_shaders "${source}.spv")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES expected_shaders)
foreach(relative IN LISTS expected_shaders)
    if(NOT EXISTS "${RUNTIME_DIR}/shader/${relative}")
        message(FATAL_ERROR
            "developer runtime is missing shader/${relative}")
    endif()
endforeach()

file(GLOB_RECURSE runtime_shaders "${RUNTIME_DIR}/shader/*.spv")
list(LENGTH runtime_shaders shader_count)
list(LENGTH expected_shaders expected_shader_count)
if(NOT shader_count EQUAL expected_shader_count)
    message(FATAL_ERROR
        "expected ${expected_shader_count} runtime shaders, found ${shader_count}")
endif()
