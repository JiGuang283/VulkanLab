#include "BuiltinScenes.h"

#include "Scene.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/PipelineConfigBuilder.h"
#include "core/UploadContext.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/FallbackTextures.h"
#include "render/GltfLoader.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/Texture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <utility>

namespace vkr {

namespace {

PipelineConfig makeStandardConfig(Device &device, const std::string &vp,
                                  const std::string &fp) {
    return PipelineConfigBuilder{}
        .shaders(vp, fp)
        .defaultVertexLayout()
        .msaa(device.msaaSamples())
        .pushConstant(
            {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128})
        .build();
}

} // namespace

SceneFactory vikingRoomSceneFactory(std::string tex, std::string vp,
                                    std::string fp) {
    return [tex = std::move(tex), vp = std::move(vp), fp = std::move(fp)](
               Device &device, UploadContext &upload,
               DescriptorAllocator &descriptorAllocator,
               const SceneLoadContext &loadContext)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();

        auto materialTemplate = std::make_shared<MaterialTemplate>(
            device, makeStandardConfig(device, vp, fp));
        auto fallbackTextures =
            std::make_shared<FallbackTextures>(device, upload);
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
                device, descriptorAllocator, materialTemplate,
                MaterialInstance::makeTextureSet(texture, *fallbackTextures),
                params);
        }
        auto mesh = std::shared_ptr<Mesh>(
            Mesh::fromOBJ(device, upload, "models/viking_room.obj")
                .release());

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

SceneFactory sheenChairSceneFactory(std::string vp, std::string fp) {
    return gltfSceneFactory("models/SheenChair.glb", std::move(vp),
                            std::move(fp));
}

SceneFactory gltfSceneFactory(std::string modelPath, std::string vp,
                              std::string fp,
                              std::optional<CameraPose> cameraOverride) {
    return [modelPath = std::move(modelPath), vp = std::move(vp),
            fp = std::move(fp), cameraOverride](
               Device &device, UploadContext &upload,
               DescriptorAllocator &descriptorAllocator,
               const SceneLoadContext &loadContext)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();
        auto materialTemplate = std::make_shared<MaterialTemplate>(
            device, makeStandardConfig(device, vp, fp));
        scene->addMaterialTemplate(materialTemplate);
        auto fallbackTextures =
            std::make_shared<FallbackTextures>(device, upload);
        GltfLoader::Options options{};
        options.maxTextureSize = loadContext.maxTextureSize;
        options.fallbackTextures = fallbackTextures;
        options.loadStats = loadContext.loadStats;
        auto asset = GltfLoader::load(modelPath, device, upload,
                                      descriptorAllocator, materialTemplate,
                                      options);

        for (auto &t : asset.textures)
            scene->addTexture(t);
        for (auto &m : asset.materials)
            scene->addMaterial(m);
        for (auto &mesh : asset.meshes)
            scene->addMesh(mesh);
        for (auto &o : asset.objects)
            scene->addObject(o);

        if (cameraOverride) {
            scene->initialCamera = *cameraOverride;
        } else {
            scene->initialCamera = asset.suggestedCamera.value_or(
                CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f});
        }
        return scene;
    };
}

} // namespace vkr
