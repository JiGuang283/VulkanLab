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

file(GLOB_RECURSE runtime_shaders "${RUNTIME_DIR}/shader/*.spv")
list(LENGTH runtime_shaders shader_count)
if(NOT shader_count EQUAL 15)
    message(FATAL_ERROR
        "expected 15 runtime shaders, found ${shader_count}")
endif()
