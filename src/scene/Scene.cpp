#include "Scene.h"
#include "core/Pipeline.h"
#include "render/Material.h"
#include "render/Mesh.h"

#include <glm/glm.hpp>

namespace vkr {

void Scene::addObject(SceneObject obj) {
    objects_.push_back(std::move(obj));
}

void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex,
                   Pipeline &pipeline) const {
    // All objects currently share the same pipeline — bind once.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    for (const auto &obj : objects_) {
        obj.material->bindDescriptors(cmd, pipeline.layout(), frameIndex);
        vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &obj.transform);
        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}

} // namespace vkr
