#include "BuiltinScenes.h"

#include "Scene.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/PipelineConfigBuilder.h"
#include "core/UploadContext.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/GltfPreparer.h"
#include "render/MaterialInstance.h"
#include "render/MaterialSystem.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/Texture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <utility>

namespace vkr {

namespace {

PipelineConfig makeStandardConfig(Device &device) {
    return PipelineConfigBuilder{}
        .defaultVertexLayout()
        .msaa(device.msaaSamples())
        .pushConstant(
            {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128})
        .build();
}

} // namespace

SceneFactory vikingRoomSceneFactory(std::string model, std::string tex) {
    return [model = std::move(model), tex = std::move(tex)](
               Device &device, UploadContext &upload,
               DescriptorAllocator &descriptorAllocator,
               MaterialSystem &materialSystem,
               const SceneLoadContext &loadContext)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();

        auto materialTemplate = std::make_shared<MaterialTemplate>(
            makeStandardConfig(device),
            materialSystem.descriptorSetLayout());
        auto texture = std::make_shared<Texture>(device, upload, tex);
        MaterialParams params;
        params.debugName = "Viking Room Material";
        std::shared_ptr<MaterialInstance> material;
        {
            ScopedLoadTimer materialTimer(
                loadContext.loadStats
                    ? &loadContext.loadStats->materialSetupMs
                    : nullptr);
            material = std::make_shared<MaterialInstance>(
                materialSystem, materialTemplate,
                MaterialInstance::makeTextureSet(texture, materialSystem),
                params);
        }
        auto mesh = std::shared_ptr<Mesh>(
            Mesh::fromOBJ(device, upload, model).release());

        scene->addMaterialTemplate(materialTemplate);
        scene->addTexture(texture);
        scene->addMaterial(material);
        scene->addMesh(mesh);
        scene->addObject({mesh, material, glm::mat4(1.0f)});

        scene->setUpdateFn([](Scene &s, float, float time) {
            if (s.objects().empty())
                return;
            s.objects()[0].transform =
                glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                            glm::vec3(0.0f, 0.0f, 1.0f));
        });
        scene->initialCamera = CameraPose{{2.0f, 2.0f, 2.0f}, -135.0f, -30.0f};
        return scene;
    };
}

ScenePrepareFactory gltfSceneFactory(std::string modelPath,
                                     std::optional<CameraPose> cameraOverride) {
    return [modelPath = std::move(modelPath), cameraOverride](
               const SceneLoadContext &loadContext,
               const CancellationToken &cancellation,
               SceneLoadProgress &progress) -> PreparedModelData {
        GltfPreparer::Options options{};
        options.maxTextureSize = loadContext.maxTextureSize;
        options.derivedTextureCachePath =
            loadContext.derivedTextureCachePath;
        options.projectId = loadContext.projectId;
        options.sceneId = loadContext.modelId.empty()
                              ? loadContext.sceneId
                              : loadContext.modelId;
        options.profileId = loadContext.profileId;
        options.textureTranscodeTarget =
            loadContext.textureTranscodeTarget;
        options.requireDerivedTextures =
            loadContext.requireDerivedTextures;
        options.loadStats = loadContext.loadStats;
        options.cameraOverride = cameraOverride;
        return GltfPreparer::prepare(modelPath, options, cancellation,
                                     &progress);
    };
}

} // namespace vkr
