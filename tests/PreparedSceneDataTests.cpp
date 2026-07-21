#include "render/TangentGenerator.h"
#include "render/TextureData.h"
#include "scene/PreparedSceneData.h"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

void runDerivedTextureManifestTests();
void runSceneCatalogTests();
void runSceneImportServiceTests();
void runTextureCachePipelineTests();
void runArtifactStatusTests();
void runArtifactIndexTests();
void runArtifactCachePrunerTests();
void runRuntimePackageTests();
void runAssetImportManagerTests();
void runAssetLoadCoordinatorTests();
void runSceneCatalogEditorTests();
void runRuntimeCommandDispatcherTests();
void runRuntimeControlProtocolTests();
void runBuildInfoTests();
void runDiagnosticsConfigTests();
void runSubmissionSerialTrackerTests();
void runCaptureTests();
void runRenderTestSpecTests();
void runImageComparatorTests();
void runManagedProcessWin32Tests();
void runDirectionalShadowTests();
void runRenderViewTests();
void runRenderResourceRegistryTests();

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testPreparedReferences() {
    vkr::PreparedSceneData scene;
    auto image = std::make_shared<vkr::PreparedImage>();
    image->width = 1;
    image->height = 1;
    image->pixels = {255, 255, 255, 255};
    scene.textures.push_back({image});

    vkr::PreparedMaterial material;
    material.textureIndices[vkr::indexOf(
        vkr::MaterialTextureSlot::BaseColor)] = 0;
    scene.materials.push_back(std::move(material));

    vkr::PreparedMesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.bounds.valid = true;
    mesh.bounds.min = {0.0f, 0.0f, 0.0f};
    mesh.bounds.max = {1.0f, 1.0f, 0.0f};
    scene.meshes.push_back(std::move(mesh));
    scene.objects.push_back({0, 0, glm::mat4(1.0f)});

    require(scene.objects[0].meshIndex < scene.meshes.size(),
            "prepared object mesh index is invalid");
    require(scene.objects[0].materialIndex >= 0 &&
                static_cast<size_t>(scene.objects[0].materialIndex) <
                    scene.materials.size(),
            "prepared object material index is invalid");
    require(scene.materials[0].textureIndices[vkr::indexOf(
                vkr::MaterialTextureSlot::BaseColor)] == 0,
            "prepared material texture index changed");
    require(scene.meshes[0].bounds.valid,
            "prepared mesh bounds were not retained");
}

void testSceneTypeDefaults() {
    const vkr::Bounds bounds;
    require(!bounds.valid, "default bounds unexpectedly became valid");
    require(bounds.min == glm::vec3(0.0f) &&
                bounds.max == glm::vec3(0.0f) &&
                bounds.center == glm::vec3(0.0f) && bounds.radius == 0.0f,
            "default bounds values changed");

    const vkr::CameraPose camera;
    require(camera.position == glm::vec3(2.0f, 2.0f, 2.0f) &&
                camera.yaw == -135.0f && camera.pitch == -30.0f,
            "default camera pose changed");
}

void testTangents() {
    std::vector<vkr::Vertex> vertices(3);
    vertices[0].pos = {0.0f, 0.0f, 0.0f};
    vertices[1].pos = {1.0f, 0.0f, 0.0f};
    vertices[2].pos = {0.0f, 1.0f, 0.0f};
    vertices[0].texCoord = {0.0f, 0.0f};
    vertices[1].texCoord = {1.0f, 0.0f};
    vertices[2].texCoord = {0.0f, 1.0f};
    for (auto &vertex : vertices)
        vertex.normal = {0.0f, 0.0f, 1.0f};

    vkr::generateTangents(vertices, {0, 1, 2});
    for (const auto &vertex : vertices) {
        require(std::isfinite(vertex.tangent.x) &&
                    std::isfinite(vertex.tangent.y) &&
                    std::isfinite(vertex.tangent.z) &&
                    std::isfinite(vertex.tangent.w),
                "generated tangent is not finite");
        require(std::abs(glm::length(glm::vec3(vertex.tangent)) - 1.0f) <
                    1.0e-4f,
                "generated tangent is not normalized");
    }
}

void testTextureResize() {
    const std::vector<uint8_t> pixels = {
        255, 0,   0,   255, 0,   255, 0,   255,
        0,   0,   255, 255, 255, 255, 255, 255,
    };
    const auto resized =
        vkr::resizeRgba8Bilinear(pixels.data(), 2, 2, 1, 1);
    require(resized.size() == 4, "resized RGBA8 byte count is wrong");
    require(resized[3] == 255, "RGBA8 resize did not preserve alpha");

    uint32_t width = 0;
    uint32_t height = 0;
    require(vkr::limitedTextureExtent(4096, 2048, 1024, width, height),
            "texture extent should have been limited");
    require(width == 1024 && height == 512,
            "texture limit did not preserve aspect ratio");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 &&
        std::string_view(argv[1]) == "--managed-process-echo") {
#ifdef _WIN32
        char *value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, "VKR_PROCESS_TEST") != 0 ||
            !value)
            return 3;
        std::cout << value << '\n';
        std::free(value);
#else
        const char *value = std::getenv("VKR_PROCESS_TEST");
        if (!value)
            return 3;
        std::cout << value << '\n';
#endif
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--managed-process-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }

    try {
        testPreparedReferences();
        testSceneTypeDefaults();
        testTangents();
        testTextureResize();
        runDerivedTextureManifestTests();
        runSceneCatalogTests();
        runSceneImportServiceTests();
        runTextureCachePipelineTests();
        runArtifactStatusTests();
        runArtifactIndexTests();
        runArtifactCachePrunerTests();
        runRuntimePackageTests();
        runAssetImportManagerTests();
        runAssetLoadCoordinatorTests();
        runSceneCatalogEditorTests();
        runRuntimeCommandDispatcherTests();
        runRuntimeControlProtocolTests();
        runBuildInfoTests();
        runDiagnosticsConfigTests();
        runSubmissionSerialTrackerTests();
        runCaptureTests();
        runRenderTestSpecTests();
        runImageComparatorTests();
        runManagedProcessWin32Tests();
        runDirectionalShadowTests();
        runRenderViewTests();
        runRenderResourceRegistryTests();
        std::cout << "VulkanLab CPU tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "VulkanLab CPU tests failed: " << error.what() << '\n';
        return 1;
    }
}
