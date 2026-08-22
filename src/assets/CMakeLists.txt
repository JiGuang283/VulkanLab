set(VKL_ASSET_CORE_SOURCES
    AssetValidation.cpp
    ArtifactCachePruner.cpp
    ArtifactIndex.cpp
    ArtifactStatus.cpp
    CacheMutationLock.cpp
    ContentHash.cpp
    DerivedAssetPaths.cpp
    DerivedEnvironmentManifest.cpp
    DerivedTextureManifest.cpp
    ProjectContext.cpp
    RuntimePackage.cpp
    SceneCatalog.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING OR VKL_BUILD_ASSET_TOOL OR BUILD_TESTING)
    list(APPEND VKL_ASSET_CORE_SOURCES
        SceneCatalogEditor.cpp
        SceneCatalogStore.cpp
        ModelImportService.cpp
    )
endif()

add_library(vkl_asset_core STATIC ${VKL_ASSET_CORE_SOURCES})
add_library(VulkanLab::AssetCore ALIAS vkl_asset_core)
target_include_directories(vkl_asset_core PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_asset_core
    PUBLIC
        vkl_build_options
        vkl_glm
        vkl_scene_data
    PRIVATE
        vkl_shader_catalog
        vkl_json
        bcrypt
)

set(VKL_ASSET_RUNTIME_SOURCES
    AssetLoadCoordinator.cpp
    DerivedEnvironmentCache.cpp
    DerivedTextureCache.cpp
    EnvironmentLoadManager.cpp
)
if(VKL_ENABLE_ASSET_AUTHORING)
    list(APPEND VKL_ASSET_RUNTIME_SOURCES AssetImportManager.cpp)
else()
    list(APPEND VKL_ASSET_RUNTIME_SOURCES AssetImportManagerDisabled.cpp)
endif()

add_library(vkl_asset_runtime STATIC ${VKL_ASSET_RUNTIME_SOURCES})
add_library(VulkanLab::AssetRuntime ALIAS vkl_asset_runtime)
target_include_directories(vkl_asset_runtime PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_asset_runtime
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_gpu_runtime
        vkl_glm
        vkl_json
        vkl_vma_headers
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        vkl_tracy
        ktx_read
)
