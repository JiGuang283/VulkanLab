set(VKL_SCENE_RUNTIME_SOURCES
    GltfModelPrepareFactory.cpp
    GltfPreparer.cpp
    Camera.cpp
    Scene.cpp
    AssetRepository.cpp
    EnvironmentGpuBuilder.cpp
    EnvironmentAssetRepository.cpp
    ModelGpuBuilder.cpp
    ModelLight.cpp
    ModelSourceResolver.cpp
    PrimitiveMeshGenerator.cpp
    PrimitiveModelFactory.cpp
    RuntimeWorld.cpp
    TransformMath.cpp
    SceneLoadManager.cpp
    SceneLoadTask.cpp
    SceneRegistryBuilder.cpp
    SceneRuntimeCoordinator.cpp
)

add_library(vkl_scene_runtime STATIC ${VKL_SCENE_RUNTIME_SOURCES})
add_library(VulkanLab::SceneRuntime ALIAS vkl_scene_runtime)
target_include_directories(vkl_scene_runtime PUBLIC "${VKL_SOURCE_ROOT}")
target_link_libraries(vkl_scene_runtime
    PUBLIC
        vkl_build_options
        vkl_asset_core
        vkl_asset_runtime
        vkl_gpu_runtime
        vkl_renderer_runtime
        vkl_scene_data
        vkl_glm
        Vulkan::Vulkan
    PRIVATE
        vkl_foundation
        vkl_gltf_parser
        vkl_image_codecs
        vkl_tracy
)
