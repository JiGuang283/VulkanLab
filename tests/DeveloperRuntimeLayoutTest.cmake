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

if(NOT EXISTS "${RUNTIME_DIR}/shader/manifest.json")
    message(FATAL_ERROR "developer runtime is missing shader/manifest.json")
endif()
if(NOT DEFINED EXPECTED_SHADER_LIST OR
   NOT EXISTS "${EXPECTED_SHADER_LIST}")
    message(FATAL_ERROR "expected shader output list was not provided")
endif()
file(STRINGS "${EXPECTED_SHADER_LIST}" expected_shaders ENCODING UTF-8)
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
