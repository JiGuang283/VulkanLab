add_executable(VulkanLabCpuTests
    PreparedSceneDataTests.cpp
    DerivedTextureManifestTests.cpp
    SceneCatalogTests.cpp
    SceneImportServiceTests.cpp
    TextureCachePipelineTests.cpp
    ArtifactStatusTests.cpp
    ArtifactIndexTests.cpp
    ArtifactCachePrunerTests.cpp
    RuntimePackageTests.cpp
    AssetImportManagerTests.cpp
    AssetLoadCoordinatorTests.cpp
    SceneCatalogEditorTests.cpp
    RuntimeCommandDispatcherTests.cpp
)
target_link_libraries(VulkanLabCpuTests PRIVATE
    vkl_build_options
    vkl_asset_core
    vkl_asset_runtime
    vkl_asset_tool_core
    vkl_control
    vkl_engine
)
add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)

if(WIN32)
    add_test(
        NAME VulkanLabAssetToolCatalogImport
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_catalog_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCatalogImportTest.cmake"
    )
    add_test(
        NAME VulkanLabAssetToolTextureCache
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_texture_cache_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolTextureCacheTest.cmake"
    )
    add_test(
        NAME VulkanLabAssetToolCook
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_cook_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCookTest.cmake"
    )
    add_test(
        NAME VulkanLabDeveloperRuntimeLayout
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/DeveloperRuntimeLayoutTest.cmake"
    )
endif()
