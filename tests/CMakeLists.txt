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
    RuntimeControlProtocolTests.cpp
    BuildInfoTests.cpp
    DiagnosticsConfigTests.cpp
    ValidationProfileTests.cpp
    SubmissionSerialTrackerTests.cpp
    CaptureTests.cpp
    RenderTestSpecTests.cpp
    ImageComparatorTests.cpp
    ManagedProcessWin32Tests.cpp
    DirectionalShadowTests.cpp
    RenderViewTests.cpp
    RenderResourceRegistryTests.cpp
    PipelineKeyTests.cpp
    ShaderRegistryTests.cpp
    ShaderContractTests.cpp
    GpuPassProfilerTests.cpp
    GpuDebugUtilsTests.cpp
    EnvironmentCacheTests.cpp
)
target_link_libraries(VulkanLabCpuTests PRIVATE
    vkl_build_options
    vkl_asset_core
    vkl_asset_runtime
    vkl_asset_tool_core
    vkl_control
    vkl_engine
    vkl_render_test_core
    VulkanLab::SpirvReflect
)
add_dependencies(VulkanLabCpuTests VulkanLabShaders)
target_compile_definitions(VulkanLabCpuTests PRIVATE
    VKL_TEST_SHADER_ROOT="$<TARGET_FILE_DIR:VulkanLab>/shader"
)
add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)
set_tests_properties(VulkanLabCpuTests PROPERTIES LABELS "unit")

if(WIN32)
    add_test(
        NAME VulkanLabAssetToolCatalogImport
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_catalog_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCatalogImportTest.cmake"
    )
    set_tests_properties(VulkanLabAssetToolCatalogImport PROPERTIES
        LABELS "unit;asset")
    add_test(
        NAME VulkanLabAssetToolTextureCache
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_texture_cache_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolTextureCacheTest.cmake"
    )
    set_tests_properties(VulkanLabAssetToolTextureCache PROPERTIES
        LABELS "unit;asset")
    add_test(
        NAME VulkanLabAssetToolCook
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/asset_tool_cook_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCookTest.cmake"
    )
    set_tests_properties(VulkanLabAssetToolCook PROPERTIES
        LABELS "unit;asset;package")
    add_test(
        NAME VulkanLabDeveloperRuntimeLayout
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/DeveloperRuntimeLayoutTest.cmake"
    )
    set_tests_properties(VulkanLabDeveloperRuntimeLayout PROPERTIES
        LABELS "unit;package")

    function(vulkanlab_add_visual_test test_name spec_name)
        add_test(
            NAME ${test_name}
            COMMAND $<TARGET_FILE:VulkanLabRenderTest> run
                --runtime $<TARGET_FILE:VulkanLab>
                --project "${PROJECT_SOURCE_DIR}"
                --spec "${CMAKE_CURRENT_SOURCE_DIR}/render/${spec_name}"
                --output "${PROJECT_BINARY_DIR}/render-test-results"
                --operation-timeout-ms 600000
        )
        set_tests_properties(${test_name} PROPERTIES
            LABELS "gpu;visual"
            RESOURCE_LOCK VulkanLabGpu
            SKIP_RETURN_CODE 125
            TIMEOUT 660
        )
    endfunction()

    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeViking viking_legacy_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeSheenNormal sheen_normal_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeSheenBaseColor sheen_basecolor_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeVikingPbrShadow viking_pbr_shadow_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeVikingDebugShadow viking_debug_shadow_smoke.json)
    add_test(
        NAME VulkanLabRenderSmokeEnvironmentIbl
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_TOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DRENDER_TEST=$<TARGET_FILE:VulkanLabRenderTest>"
            "-DRUNTIME=$<TARGET_FILE:VulkanLab>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/environment_ibl_visual_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/EnvironmentIblVisualTest.cmake"
    )
    set_tests_properties(VulkanLabRenderSmokeEnvironmentIbl PROPERTIES
        LABELS "asset;gpu;visual"
        RESOURCE_LOCK VulkanLabGpu
        TIMEOUT 660
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/goldens/viking-legacy.png"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/goldens/viking-legacy.json")
        vulkanlab_add_visual_test(
            VulkanLabRenderGoldenViking viking_legacy_golden.json)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/goldens/viking-pbr-shadow.png"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/goldens/viking-pbr-shadow.json")
        vulkanlab_add_visual_test(
            VulkanLabRenderGoldenVikingPbrShadow
            viking_pbr_shadow_golden.json)
    endif()
endif()
