set(VKL_TEST_WORK_ROOT "${PROJECT_BINARY_DIR}/test-work")
set(VKL_TEST_RESULTS_ROOT "${PROJECT_BINARY_DIR}/test-results")
file(MAKE_DIRECTORY "${VKL_TEST_WORK_ROOT}/unit")

add_executable(VulkanLabCpuTests
    PreparedModelDataTests.cpp
    DerivedTextureManifestTests.cpp
    SceneCatalogTests.cpp
    ModelImportServiceTests.cpp
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
    RenderResourcePoolTests.cpp
    PipelineKeyTests.cpp
    ShaderRegistryTests.cpp
    ShaderContractTests.cpp
    GpuPassProfilerTests.cpp
    GpuDebugUtilsTests.cpp
    EnvironmentCacheTests.cpp
)
set_target_properties(VulkanLabCpuTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test-bin/$<CONFIG>"
)
target_link_libraries(VulkanLabCpuTests PRIVATE
    vkl_project_options
    vkl_project_warnings
    vkl_asset_core
    # The combined test executable exercises both offline KTX writing and
    # runtime KTX reading. Keep the complete provider before ktx_read's
    # transitive dependency so MSVC does not extract duplicate KTX2 symbols.
    vkl_asset_tool_core
    vkl_asset_runtime
    vkl_capture
    vkl_control
    vkl_foundation
    vkl_gpu_runtime
    vkl_renderer_runtime
    vkl_render_test_core
    VulkanLab::SpirvReflect
)
add_dependencies(VulkanLabCpuTests VulkanLabShaders)
target_compile_definitions(VulkanLabCpuTests PRIVATE
    VKL_TEST_SHADER_ROOT="$<TARGET_FILE_DIR:VulkanLab>/shader"
)
add_test(NAME VulkanLabCpuTests COMMAND VulkanLabCpuTests)
set_tests_properties(VulkanLabCpuTests PROPERTIES
    LABELS "unit"
    WORKING_DIRECTORY "${VKL_TEST_WORK_ROOT}/unit"
)

if(WIN32)
    add_test(
        NAME VulkanLabAssetToolCatalogImport
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${VKL_TEST_WORK_ROOT}/asset-tool-catalog"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCatalogImportTest.cmake"
    )
    set_tests_properties(VulkanLabAssetToolCatalogImport PROPERTIES
        LABELS "unit;asset")
    add_test(
        NAME VulkanLabAssetToolTextureCache
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DTEST_ROOT=${VKL_TEST_WORK_ROOT}/asset-tool-texture-cache"
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
            "-DTEST_ROOT=${VKL_TEST_WORK_ROOT}/asset-tool-cook"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetToolCookTest.cmake"
    )
    set_tests_properties(VulkanLabAssetToolCook PROPERTIES
        LABELS "unit;asset;package")
    add_test(
        NAME VulkanLabDeveloperRuntimeLayout
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:VulkanLab>"
            "-DEXPECTED_SHADER_LIST=${VKL_SHADER_EXPECTED_LIST}"
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
                --output "${VKL_TEST_RESULTS_ROOT}/render"
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
        VulkanLabRenderSmokeRendererLegacy renderer_smoke_legacy.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeSheenNormal sheen_normal_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeSheenBaseColor sheen_basecolor_smoke.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeRendererPbrShadow renderer_smoke_pbr_shadow.json)
    vulkanlab_add_visual_test(
        VulkanLabRenderSmokeRendererDebugShadow renderer_smoke_debug_shadow.json)
    add_test(
        NAME VulkanLabRenderSmokeEnvironmentIbl
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_TOOL=$<TARGET_FILE:VulkanLabAssetTool>"
            "-DRENDER_TEST=$<TARGET_FILE:VulkanLabRenderTest>"
            "-DRUNTIME=$<TARGET_FILE:VulkanLab>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${VKL_TEST_WORK_ROOT}/environment-ibl-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/EnvironmentIblVisualTest.cmake"
    )
    set_tests_properties(VulkanLabRenderSmokeEnvironmentIbl PROPERTIES
        LABELS "asset;gpu;visual"
        RESOURCE_LOCK VulkanLabGpu
        TIMEOUT 660
    )

endif()
