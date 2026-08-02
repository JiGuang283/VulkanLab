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

file(REMOVE_RECURSE "${TEST_ROOT}")
set(project "${TEST_ROOT}/project")
set(package "${TEST_ROOT}/package")
file(MAKE_DIRECTORY "${project}/assets/scenes")

file(WRITE "${project}/assets/catalog.json" [=[
{
  "schemaVersion": 3,
  "projectId": "cook-test",
  "defaultImportProfile": "desktop-512",
  "importProfiles": {
    "desktop-512": {
      "textureLimit": 512,
      "textureEncoder": "bc7",
      "qualityPreset": "development"
    }
  },
  "environmentProfiles": {
    "tiny-ibl": {
      "radianceSize": 4,
      "irradianceSize": 2,
      "prefilteredSize": 4,
      "brdfLutSize": 4,
      "diffuseSamples": 8,
      "specularSamples": 8,
      "brdfSamples": 8
    }
  },
  "models": [],
  "scenes": [{
    "id": "native-scene",
    "displayName": "Native Scene",
    "source": "assets/scenes/native-scene.vkscene.json",
    "optional": false
  }],
  "environments": []
}
]=])

file(WRITE "${project}/assets/scenes/native-scene.vkscene.json" [=[
{
  "schemaVersion": 2,
  "id": "native-scene",
  "displayName": "Native Scene",
  "activeCamera": "00000000-0000-4000-8000-000000000001",
  "ambient": {"color": [0.03, 0.03, 0.03], "intensity": 1.0},
  "environment": null,
  "entities": [{
    "id": "00000000-0000-4000-8000-000000000001",
    "name": "Camera",
    "parent": null,
    "enabled": true,
    "transform": {
      "translation": [0.0, -5.0, 2.0],
      "rotation": [0.5606289, 0.0, 0.0, 0.8280673],
      "scale": [1.0, 1.0, 1.0]
    },
    "components": {"camera": {
      "verticalFovRadians": 1.0471976,
      "nearPlane": 0.05,
      "farPlane": 1000.0
    }}
  }]
}
]=])

execute_process(
    COMMAND "${TOOL}" cook
        --project "${project}"
        --runtime-dir "${RUNTIME_DIR}"
        --output "${package}"
        --model-id legacy-model
    RESULT_VARIABLE legacy_result
    OUTPUT_VARIABLE legacy_output
    ERROR_VARIABLE legacy_error
)
if(legacy_result EQUAL 0 OR
   NOT legacy_error MATCHES "no longer accepted by cook")
    message(FATAL_ERROR
        "cook did not reject the legacy model-root CLI\n${legacy_output}\n${legacy_error}")
endif()

# The ordinary test build intentionally contains development features. A real
# package must use the separate windows-msvc-runtime Release preset.
execute_process(
    COMMAND "${TOOL}" cook
        --project "${project}"
        --runtime-dir "${RUNTIME_DIR}"
        --output "${package}"
        --scene-id native-scene
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
)
if(runtime_result EQUAL 0 OR
   NOT runtime_error MATCHES
       "runtime build (contains development-only features|must be Release)")
    message(FATAL_ERROR
        "cook accepted a development runtime\n${runtime_output}\n${runtime_error}")
endif()
if(EXISTS "${package}")
    message(FATAL_ERROR "failed cook published an output directory")
endif()
