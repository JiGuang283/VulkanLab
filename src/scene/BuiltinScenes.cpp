#include "BuiltinScenes.h"

#include "Scene.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/PipelineConfigBuilder.h"
#include "render/GltfLoader.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Renderer.h"
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
               Device &device, FrameSync &frameSync, Renderer &renderer,
               DescriptorAllocator &descriptorAllocator)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();

        auto texture = std::make_shared<Texture>(device, frameSync, tex);
        auto material = std::make_shared<Material>(
            device, renderer, descriptorAllocator, *texture,
            makeStandardConfig(device, vp, fp));
        auto mesh = std::shared_ptr<Mesh>(
            Mesh::fromOBJ(device, frameSync, "models/viking_room.obj")
                .release());

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
                              std::string fp) {
    return [modelPath = std::move(modelPath), vp = std::move(vp),
            fp = std::move(fp)](Device &device, FrameSync &frameSync,
                                Renderer            &renderer,
                                DescriptorAllocator &descriptorAllocator)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();
        auto baseCfg = makeStandardConfig(device, vp, fp);
        auto asset = GltfLoader::load(modelPath, device, frameSync, renderer,
                                      descriptorAllocator, baseCfg);

        for (auto &t : asset.textures)
            scene->addTexture(t);
        for (auto &m : asset.materials)
            scene->addMaterial(m);
        for (auto &mesh : asset.meshes)
            scene->addMesh(mesh);
        for (auto &o : asset.objects)
            scene->addObject(o);

        scene->initialCamera = asset.suggestedCamera.value_or(
            CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f});
        return scene;
    };
}

} // namespace vkr
