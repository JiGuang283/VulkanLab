if(NOT DEFINED TOOL OR NOT EXISTS "${TOOL}")
    message(FATAL_ERROR "VulkanLabAssetTool was not provided")
endif()
if(NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_ROOT was not provided")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/project/assets")
file(MAKE_DIRECTORY "${TEST_ROOT}/source/textures/deep")
file(WRITE "${TEST_ROOT}/project/assets/catalog.json" [=[
{
  "schemaVersion": 1,
  "projectId": "cli-import-test",
  "defaultImportProfile": "desktop_512",
  "importProfiles": {"desktop_512": {"textureLimit": 512}},
  "scenes": [
    {"id":"builtin","displayName":"Builtin","type":"builtin","builtinFactory":"viking_room"}
  ]
}
]=])
file(WRITE "${TEST_ROOT}/source/mesh.bin" "mesh")
file(WRITE "${TEST_ROOT}/source/textures/deep/base.png" "png")
file(WRITE "${TEST_ROOT}/source/scene.gltf" [=[
{
  "asset": {"version": "2.0"},
  "buffers": [{"uri":"mesh.bin","byteLength":4}],
  "images": [{"uri":"textures/deep/base.png"}]
}
]=])

execute_process(
    COMMAND "${TOOL}" catalog add
        --project "${TEST_ROOT}/project"
        --source "${TEST_ROOT}/source/scene.gltf"
        --display-name "CLI Imported"
        --scene-id "cli-imported"
        --profile "desktop_512"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "catalog add failed (${result})\n${output}\n${error}")
endif()

set(imported "${TEST_ROOT}/project/models/imported/cli-imported")
foreach(path
        "${imported}/scene.gltf"
        "${imported}/mesh.bin"
        "${imported}/textures/deep/base.png")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Imported dependency is missing: ${path}")
    endif()
endforeach()
file(READ "${TEST_ROOT}/project/assets/catalog.json" catalog)
string(FIND "${catalog}" "\"id\": \"cli-imported\"" scene_id_position)
if(scene_id_position EQUAL -1)
    message(FATAL_ERROR "Catalog does not contain the imported scene")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
