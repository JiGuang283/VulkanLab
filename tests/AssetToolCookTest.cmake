if(NOT DEFINED TOOL OR NOT EXISTS "${TOOL}")
    message(FATAL_ERROR "VulkanLabAssetTool was not provided")
endif()
if(NOT DEFINED RUNTIME_DIR OR
   NOT EXISTS "${RUNTIME_DIR}/VulkanLab.exe")
    message(FATAL_ERROR "VulkanLab runtime directory was not provided")
endif()
if(NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_ROOT was not provided")
endif()
if(NOT DEFINED SOURCE_DIR OR
   NOT EXISTS "${SOURCE_DIR}/models/viking_room.obj" OR
   NOT EXISTS "${SOURCE_DIR}/textures/viking_room.png")
    message(FATAL_ERROR "VulkanLab source assets were not provided")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
set(project "${TEST_ROOT}/project")
set(cache "${TEST_ROOT}/cache")
set(package "${TEST_ROOT}/package")
file(MAKE_DIRECTORY
    "${project}/assets" "${project}/models" "${project}/textures")
file(COPY "${SOURCE_DIR}/models/viking_room.obj"
     DESTINATION "${project}/models")
file(COPY "${SOURCE_DIR}/textures/viking_room.png"
     DESTINATION "${project}/textures")
file(WRITE "${project}/assets/catalog.json" [=[
{
  "schemaVersion": 1,
  "projectId": "cook-test",
  "defaultImportProfile": "desktop-512",
  "importProfiles": {
    "desktop-512": {
      "textureLimit": 512,
      "textureEncoder": "uastc",
      "qualityPreset": "development"
    }
  },
  "scenes": [
    {
      "id": "viking-room",
      "displayName": "Viking Room",
      "type": "builtin",
      "builtinFactory": "viking_room",
      "importProfile": "desktop-512"
    },
    {
      "id": "tiny-scene",
      "displayName": "Tiny Scene",
      "source": "models/tiny.gltf",
      "importProfile": "desktop-512"
    }
  ]
}
]=])
file(WRITE "${project}/models/mesh.bin" "mesh")
file(WRITE "${project}/models/unused.png" "must not be packaged")
file(WRITE "${project}/models/tiny.gltf" [=[
{
  "asset": {"version": "2.0"},
  "buffers": [{"uri": "mesh.bin", "byteLength": 4}],
  "images": [{
    "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
  }],
  "samplers": [{"wrapS": 10497, "wrapT": 10497}],
  "textures": [{"source": 0, "sampler": 0}],
  "materials": [{"pbrMetallicRoughness": {
    "baseColorTexture": {"index": 0}
  }}]
}
]=])

set(cook_command
    "${TOOL}" cook
    --project "${project}"
    --cache-root "${cache}"
    --runtime-dir "${RUNTIME_DIR}"
    --output "${package}"
    --platform windows-x64
    --profile desktop-512
    --scene-id viking-room
    --scene-id tiny-scene)

execute_process(
    COMMAND ${cook_command}
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error
)
if(missing_result EQUAL 0 OR
   NOT missing_error MATCHES "artifacts are not Ready")
    message(FATAL_ERROR
        "cook accepted missing artifacts (${missing_result})\n${missing_output}\n${missing_error}")
endif()
if(EXISTS "${package}")
    message(FATAL_ERROR "failed cook published an output directory")
endif()

execute_process(
    COMMAND "${TOOL}" texture-cache build
        --project "${project}"
        --scene-id tiny-scene
        --profile desktop-512
        --cache-root "${cache}"
        --workers 1
        --memory-budget-mib 128
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "could not build cook fixture (${build_result})\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${TOOL}" cook
        --project "${project}"
        --cache-root "${cache}"
        --runtime-dir "${RUNTIME_DIR}"
        --output "${cache}/nested-package"
        --platform windows-x64
        --profile desktop-512
        --scene-id tiny-scene
    RESULT_VARIABLE overlap_result
    OUTPUT_VARIABLE overlap_output
    ERROR_VARIABLE overlap_error
)
if(overlap_result EQUAL 0 OR NOT overlap_error MATCHES "overlaps an input root")
    message(FATAL_ERROR
        "cook accepted an output inside its cache (${overlap_result})\n${overlap_output}\n${overlap_error}")
endif()

execute_process(
    COMMAND ${cook_command}
    RESULT_VARIABLE cook_result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT cook_result EQUAL 0)
    message(FATAL_ERROR
        "cook failed (${cook_result})\n${cook_output}\n${cook_error}")
endif()

foreach(required
        "VulkanLab.exe"
        "assets/catalog.json"
        "models/tiny.gltf"
        "models/mesh.bin"
        "models/viking_room.obj"
        "textures/viking_room.png"
        "runtime_assets/artifact_index.json"
        "runtime_assets/manifests/tiny-scene/desktop-512.json"
        "package_manifest.json")
    if(NOT EXISTS "${package}/${required}")
        message(FATAL_ERROR "cooked package is missing ${required}")
    endif()
endforeach()
if(EXISTS "${package}/models/unused.png" OR
   EXISTS "${package}/shader/debug")
    message(FATAL_ERROR "cooked package contains an unused source asset")
endif()
foreach(unexpected
        "VulkanLabRenderTest.exe"
        "tests"
        "goldens"
        "artifacts/captures"
        "doc")
    if(EXISTS "${package}/${unexpected}")
        message(FATAL_ERROR
            "cooked package contains development-only ${unexpected}")
    endif()
endforeach()
file(GLOB_RECURSE packaged_shader_sources
    "${package}/*.vert" "${package}/*.frag" "${package}/*.glsl")
if(packaged_shader_sources)
    message(FATAL_ERROR
        "cooked package contains source shaders: ${packaged_shader_sources}")
endif()
file(GLOB_RECURSE packaged_images
    "${package}/*.png" "${package}/*.jpg" "${package}/*.jpeg")
list(REMOVE_ITEM packaged_images "${package}/textures/viking_room.png")
if(packaged_images)
    message(FATAL_ERROR
        "cooked package contains unexpected source images: ${packaged_images}")
endif()
file(GLOB_RECURSE packaged_shaders "${package}/*.spv")
list(LENGTH packaged_shaders shader_count)
if(NOT shader_count EQUAL 15)
    message(FATAL_ERROR "expected 15 runtime shaders, found ${shader_count}")
endif()

file(READ "${package}/package_manifest.json" first_manifest)
string(JSON first_file_count LENGTH "${first_manifest}" files)
set(first_paths)
math(EXPR first_last "${first_file_count} - 1")
foreach(index RANGE 0 ${first_last})
    string(JSON path GET "${first_manifest}" files ${index} path)
    list(APPEND first_paths "${path}")
endforeach()

execute_process(
    COMMAND ${cook_command}
    RESULT_VARIABLE recook_result
    OUTPUT_VARIABLE recook_output
    ERROR_VARIABLE recook_error
)
if(NOT recook_result EQUAL 0)
    message(FATAL_ERROR
        "repeat cook failed (${recook_result})\n${recook_output}\n${recook_error}")
endif()
file(READ "${package}/package_manifest.json" second_manifest)
string(JSON second_file_count LENGTH "${second_manifest}" files)
if(NOT second_file_count EQUAL first_file_count)
    message(FATAL_ERROR "repeat cook changed the package file count")
endif()
set(second_paths)
math(EXPR second_last "${second_file_count} - 1")
foreach(index RANGE 0 ${second_last})
    string(JSON path GET "${second_manifest}" files ${index} path)
    list(APPEND second_paths "${path}")
endforeach()
if(NOT second_paths STREQUAL first_paths)
    message(FATAL_ERROR "repeat cook changed the package file set")
endif()

execute_process(
    COMMAND "${TOOL}" package verify --path "${package}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error
)
if(NOT verify_result EQUAL 0 OR
   NOT verify_output MATCHES "Package verified")
    message(FATAL_ERROR
        "package verification failed (${verify_result})\n${verify_output}\n${verify_error}")
endif()

# A failed replacement cook must leave the previously published package valid.
execute_process(
    COMMAND "${TOOL}" cook
        --project "${project}"
        --cache-root "${TEST_ROOT}/empty-cache"
        --runtime-dir "${RUNTIME_DIR}"
        --output "${package}"
        --platform windows-x64
        --profile desktop-512
        --scene-id tiny-scene
    RESULT_VARIABLE replacement_result
    OUTPUT_VARIABLE replacement_output
    ERROR_VARIABLE replacement_error
)
if(replacement_result EQUAL 0)
    message(FATAL_ERROR "replacement cook unexpectedly accepted missing artifacts")
endif()
execute_process(
    COMMAND "${TOOL}" package verify --path "${package}"
    RESULT_VARIABLE preserved_result
    OUTPUT_VARIABLE preserved_output
    ERROR_VARIABLE preserved_error
)
if(NOT preserved_result EQUAL 0)
    message(FATAL_ERROR
        "failed replacement damaged the old package\n${preserved_output}\n${preserved_error}")
endif()

# Verification cannot depend on the source project or shared derived cache.
file(REMOVE_RECURSE "${project}" "${cache}")
execute_process(
    COMMAND "${TOOL}" package verify --path "${package}"
    RESULT_VARIABLE independent_result
    OUTPUT_VARIABLE independent_output
    ERROR_VARIABLE independent_error
)
if(NOT independent_result EQUAL 0)
    message(FATAL_ERROR
        "standalone package verification failed\n${independent_output}\n${independent_error}")
endif()

file(GLOB blobs "${package}/runtime_assets/blobs/*.ktx2")
list(LENGTH blobs blob_count)
if(blob_count LESS 1)
    message(FATAL_ERROR "cooked package has no texture blob")
endif()
list(GET blobs 0 tampered_blob)
file(APPEND "${tampered_blob}" "tampered")
execute_process(
    COMMAND "${TOOL}" package verify --path "${package}"
    RESULT_VARIABLE tampered_result
    OUTPUT_VARIABLE tampered_output
    ERROR_VARIABLE tampered_error
)
if(tampered_result EQUAL 0 OR
   NOT tampered_error MATCHES "file size mismatch|file hash mismatch")
    message(FATAL_ERROR
        "tampered blob was accepted (${tampered_result})\n${tampered_output}\n${tampered_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
