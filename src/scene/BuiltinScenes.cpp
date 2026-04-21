#include "BuiltinScenes.h"

#include "Scene.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/PipelineConfig.h"
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
    PipelineConfig cfg;
    cfg.vertShaderPath = vp;
    cfg.fragShaderPath = fp;
    cfg.vertexLayout = defaultVertexLayout();
    cfg.msaaSamples = device.msaaSamples();
    cfg.pushConstants = {{VK_SHADER_STAGE_VERTEX_BIT |
                              VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, 128}};
    return cfg;
}

} // namespace

SceneFactory vikingRoomSceneFactory(std::string tex, std::string vp,
                                    std::string fp) {
    return [tex = std::move(tex), vp = std::move(vp),
            fp = std::move(fp)](Device &device, FrameSync &frameSync,
                                Renderer &renderer) -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();

        auto texture = std::make_shared<Texture>(device, frameSync, tex);
        auto material = std::make_shared<Material>(
            device, renderer, *texture, makeStandardConfig(device, vp, fp));
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

SceneFactory sheenChairSceneFactory(std::string tex, std::string vp,
                                    std::string fp) {
    return [tex = std::move(tex), vp = std::move(vp),
            fp = std::move(fp)](Device &device, FrameSync &frameSync,
                                Renderer &renderer) -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();

        auto texture = std::make_shared<Texture>(device, frameSync, tex);
        auto material = std::make_shared<Material>(
            device, renderer, *texture, makeStandardConfig(device, vp, fp));

        scene->addTexture(texture);
        scene->addMaterial(material);

        auto raw = GltfLoader::load("models/SheenChair.glb", device, frameSync);
        for (auto &rm : raw) {
            auto sp = std::shared_ptr<Mesh>(std::move(rm));
            scene->addMesh(sp);
            scene->addObject({sp, material, glm::mat4(1.0f)});
        }

        scene->initialCamera = CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f};
        return scene;
    };
}

} // namespace vkr
