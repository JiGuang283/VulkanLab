if(NOT DEFINED ASSET_TOOL OR NOT EXISTS "${ASSET_TOOL}")
    message(FATAL_ERROR "VulkanLabAssetTool was not provided")
endif()
if(NOT DEFINED RENDER_TEST OR NOT EXISTS "${RENDER_TEST}")
    message(FATAL_ERROR "VulkanLabRenderTest was not provided")
endif()
if(NOT DEFINED RUNTIME OR NOT EXISTS "${RUNTIME}")
    message(FATAL_ERROR "VulkanLab runtime was not provided")
endif()
if(NOT DEFINED SOURCE_DIR OR
   NOT EXISTS "${SOURCE_DIR}/models/viking_room.obj" OR
   NOT EXISTS "${SOURCE_DIR}/models/SheenChair.glb" OR
   NOT EXISTS "${SOURCE_DIR}/textures/viking_room.png")
    message(FATAL_ERROR "Viking Room fixture assets were not provided")
endif()
if(NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_ROOT was not provided")
endif()

set(project "${TEST_ROOT}/project")
set(spec_root "${TEST_ROOT}/specs")
set(result_root "${TEST_ROOT}/results")
if(DEFINED ENV{LOCALAPPDATA})
    set(cache_root
        "$ENV{LOCALAPPDATA}/VulkanLab/DerivedAssets/ibl-visual-test")
else()
    set(cache_root
        "$ENV{TEMP}/VulkanLab/DerivedAssets/ibl-visual-test")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}" "${cache_root}")
file(MAKE_DIRECTORY
    "${project}/assets/environments" "${project}/models"
    "${project}/textures" "${spec_root}" "${result_root}")
file(COPY "${SOURCE_DIR}/models/viking_room.obj"
     DESTINATION "${project}/models")
file(COPY "${SOURCE_DIR}/models/SheenChair.glb"
     DESTINATION "${project}/models")
file(COPY "${SOURCE_DIR}/textures/viking_room.png"
     DESTINATION "${project}/textures")

execute_process(
    COMMAND powershell -NoProfile -Command
        "[IO.File]::WriteAllBytes('${project}/assets/environments/test.hdr',[Convert]::FromBase64String('Iz9SQURJQU5DRQpGT1JNQVQ9MzItYml0X3JsZV9yZ2JlCgotWSAyICtYIDQKgCAQgYAwGIGAQCCBgFAogYBgMIGAcDiBgIBAgYCQSIE='))"
    RESULT_VARIABLE hdr_fixture_result
)
if(NOT hdr_fixture_result EQUAL 0)
    message(FATAL_ERROR "could not create IBL visual HDR fixture")
endif()

file(WRITE "${project}/assets/catalog.json" [=[
{
  "schemaVersion": 2,
  "projectId": "ibl-visual-test",
  "defaultImportProfile": "desktop_2048",
  "importProfiles": {
    "desktop_2048": {"textureLimit": 2048}
  },
  "environmentProfiles": {
    "tiny-ibl": {
      "radianceSize": 4,
      "irradianceSize": 2,
      "prefilteredSize": 4,
      "brdfLutSize": 4,
      "diffuseSamples": 16,
      "specularSamples": 16,
      "brdfSamples": 16
    }
  },
  "environments": [{
    "id": "studio",
    "displayName": "Studio",
    "source": "assets/environments/test.hdr",
    "environmentProfile": "tiny-ibl"
  }],
  "scenes": [
    {
      "id": "viking-room",
      "displayName": "Viking Room",
      "type": "builtin",
      "builtinFactory": "viking_room",
      "importProfile": "desktop_2048"
    },
    {
      "id": "sheen-chair",
      "displayName": "Sheen Chair",
      "source": "models/SheenChair.glb",
      "importProfile": "desktop_2048"
    }
  ]
}
]=])

execute_process(
    COMMAND "${ASSET_TOOL}" environment-cache build
        --project "${project}"
        --environment-id studio
        --profile tiny-ibl
        --workers 1
    RESULT_VARIABLE bake_result
    OUTPUT_VARIABLE bake_output
    ERROR_VARIABLE bake_error
)
if(NOT bake_result EQUAL 0)
    message(FATAL_ERROR
        "IBL fixture bake failed (${bake_result})\n${bake_output}\n${bake_error}")
endif()

function(write_ibl_spec path name scene shader skybox rotation
         position yaw pitch)
    file(WRITE "${path}" "{
  \"schemaVersion\": 3,
  \"name\": \"${name}\",
  \"sceneId\": \"${scene}\",
  \"profileId\": \"desktop_2048\",
  \"environmentId\": \"studio\",
  \"shader\": \"${shader}\",
  \"camera\": {
    \"position\": [${position}],
    \"yaw\": ${yaw},
    \"pitch\": ${pitch}
  },
  \"viewport\": [640, 480],
  \"fixedDelta\": 0.016666667,
  \"stableFrames\": 4,
  \"includeGui\": false,
  \"renderSettings\": {
    \"shadowsEnabled\": false,
    \"exposureEv\": 0.0,
    \"toneMapper\": \"aces\",
    \"iblEnabled\": true,
    \"skyboxEnabled\": ${skybox},
    \"environmentIntensity\": 1.0,
    \"environmentRotationRadians\": ${rotation}
  },
  \"mode\": \"smoke\",
  \"thresholds\": {
    \"minimumNonBlackRatio\": 0.005,
    \"maximumSolidColorRatio\": 0.999
  }
}
")
endfunction()

write_ibl_spec(
    "${spec_root}/skybox-rotation-0.json"
    "ibl-skybox-rotation-0" "viking-room"
    "PBR-lite NormalMapped" true 0.0 "2.0, 2.0, 2.0" -135.0 -30.0)
write_ibl_spec(
    "${spec_root}/skybox-rotation-90.json"
    "ibl-skybox-rotation-90" "viking-room"
    "PBR-lite NormalMapped" true 1.57079632679
    "2.0, 2.0, 2.0" -135.0 -30.0)
write_ibl_spec(
    "${spec_root}/debug-diffuse.json"
    "ibl-debug-diffuse" "sheen-chair"
    "Debug IBL Diffuse" false 0.0 "1.5, 1.5, 1.0" -135.0 -20.0)
write_ibl_spec(
    "${spec_root}/debug-specular.json"
    "ibl-debug-specular" "sheen-chair"
    "Debug IBL Specular" false 0.0 "1.5, 1.5, 1.0" -135.0 -20.0)

function(run_ibl_spec spec output actual_variable)
    file(MAKE_DIRECTORY "${output}")
    execute_process(
        COMMAND "${RENDER_TEST}" run
            --runtime "${RUNTIME}"
            --project "${project}"
            --spec "${spec}"
            --output "${output}"
            --operation-timeout-ms 600000
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR
            "IBL visual run failed (${run_result})\n${run_output}\n${run_error}")
    endif()
    file(GLOB actuals "${output}/*/actual.png")
    list(LENGTH actuals actual_count)
    if(NOT actual_count EQUAL 1)
        message(FATAL_ERROR
            "IBL visual run did not produce exactly one actual.png")
    endif()
    list(GET actuals 0 actual)
    set(${actual_variable} "${actual}" PARENT_SCOPE)
endfunction()

run_ibl_spec(
    "${spec_root}/skybox-rotation-0.json"
    "${result_root}/rotation-0" rotation_0_actual)
run_ibl_spec(
    "${spec_root}/skybox-rotation-90.json"
    "${result_root}/rotation-90" rotation_90_actual)
run_ibl_spec(
    "${spec_root}/debug-diffuse.json"
    "${result_root}/debug-diffuse" diffuse_actual)
run_ibl_spec(
    "${spec_root}/debug-specular.json"
    "${result_root}/debug-specular" specular_actual)

file(SHA256 "${rotation_0_actual}" rotation_0_hash)
file(SHA256 "${rotation_90_actual}" rotation_90_hash)
if(rotation_0_hash STREQUAL rotation_90_hash)
    message(FATAL_ERROR
        "rotating the environment by 90 degrees did not change the image")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}" "${cache_root}")
