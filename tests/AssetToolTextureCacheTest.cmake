if(NOT DEFINED TOOL OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TOOL and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/assets" "${TEST_ROOT}/models")

file(WRITE "${TEST_ROOT}/assets/catalog.json" [=[
{
  "schemaVersion": 1,
  "projectId": "texture-cache-test",
  "defaultImportProfile": "test-development",
  "importProfiles": {
    "test-development": {
      "textureLimit": 512,
      "textureEncoder": "uastc",
      "qualityPreset": "development"
    }
  },
  "scenes": [{
    "id": "tiny-scene",
    "displayName": "Tiny Scene",
    "source": "models/tiny.gltf",
    "importProfile": "test-development"
  }]
}
]=])

# One image is used by three semantics so the test exercises parallel work
# without carrying a binary fixture in the repository.
file(WRITE "${TEST_ROOT}/models/tiny.gltf" [=[
{
  "asset": {"version": "2.0"},
  "images": [{
    "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
  }],
  "samplers": [{"wrapS": 10497, "wrapT": 10497}],
  "textures": [{"source": 0, "sampler": 0}],
  "materials": [{
    "pbrMetallicRoughness": {
      "baseColorTexture": {"index": 0},
      "metallicRoughnessTexture": {"index": 0}
    },
    "normalTexture": {"index": 0}
  }]
}
]=])

function(run_build cache workers preset output_var)
    execute_process(
        COMMAND "${TOOL}" texture-cache build
            --project "${TEST_ROOT}"
            --scene-id tiny-scene
            --profile test-development
            --cache-root "${cache}"
            --workers "${workers}"
            --memory-budget-mib 128
            --preset "${preset}"
            --progress ndjson
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "texture cache build failed (${result})\nstdout:\n${output}\nstderr:\n${errors}")
    endif()
    string(REPLACE "\r\n" "\n" normalized "${output}")
    string(REGEX REPLACE "\n$" "" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    foreach(line IN LISTS lines)
        if(NOT line MATCHES "^\\{.*\\}$")
            message(FATAL_ERROR "stdout is not pure NDJSON: ${line}")
        endif()
    endforeach()
    set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

set(serial_cache "${TEST_ROOT}/cache-serial")
set(parallel_cache "${TEST_ROOT}/cache-parallel")
set(production_cache "${TEST_ROOT}/cache-production")
run_build("${serial_cache}" 1 development serial_output)
run_build("${parallel_cache}" 3 development parallel_output)

set(manifest_relative "manifests/tiny-scene/test-development.json")
file(READ "${serial_cache}/${manifest_relative}" serial_manifest)
file(READ "${parallel_cache}/${manifest_relative}" parallel_manifest)
if(NOT serial_manifest STREQUAL parallel_manifest)
    message(FATAL_ERROR "serial and parallel manifests differ")
endif()

run_build("${parallel_cache}" 3 development hit_output)
if(NOT hit_output MATCHES "\"encoded\":0" OR
   NOT hit_output MATCHES "\"reused\":3")
    message(FATAL_ERROR "second build did not reuse all three artifacts")
endif()

run_build("${production_cache}" 2 production production_output)
file(READ "${production_cache}/${manifest_relative}" production_manifest)
string(JSON development_key GET "${serial_manifest}" entries 0 cacheKey)
string(JSON production_key GET "${production_manifest}" entries 0 cacheKey)
if(development_key STREQUAL production_key)
    message(FATAL_ERROR "production preset reused a development cache key")
endif()
string(JSON stored_preset GET "${production_manifest}" qualityPreset)
if(NOT stored_preset STREQUAL "production")
    message(FATAL_ERROR "production preset was not recorded in the manifest")
endif()

set(failure_cache "${TEST_ROOT}/cache-failure")
execute_process(
    COMMAND "${TOOL}" texture-cache build
        --project "${TEST_ROOT}"
        --scene-id tiny-scene
        --profile test-development
        --cache-root "${failure_cache}"
        --workers 2
        --memory-budget-mib 128
        --ktx-tool "$ENV{ComSpec}"
        --progress ndjson
    RESULT_VARIABLE failure_result
    OUTPUT_VARIABLE failure_output
    ERROR_VARIABLE failure_errors
)
if(failure_result EQUAL 0)
    message(FATAL_ERROR "synthetic process failure unexpectedly succeeded")
endif()
if(NOT failure_output MATCHES "\"event\":\"failed\"" OR
   NOT failure_output MATCHES "\"exitCode\":")
    message(FATAL_ERROR "failure NDJSON omitted structured process details")
endif()
if(NOT failure_errors MATCHES "image 0.*command exit code")
    message(FATAL_ERROR "failure diagnostic omitted image or exit code")
endif()
if(EXISTS "${failure_cache}/${manifest_relative}")
    message(FATAL_ERROR "failed build published a scene manifest")
endif()
file(GLOB_RECURSE failure_temps "${failure_cache}/.vulkanlab-*")
if(failure_temps)
    message(FATAL_ERROR "failed build left temporary files: ${failure_temps}")
endif()
