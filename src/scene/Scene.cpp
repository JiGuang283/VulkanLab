#include "Scene.h"
#include "render/Material.h"
#include "render/Mesh.h"

#include <glm/glm.hpp>

namespace vkr {

void Scene::addObject(SceneObject obj) {
    objects_.push_back(std::move(obj));
}

void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex) const {
    for (const auto &obj : objects_) {
        obj.material->bind(cmd, frameIndex);
        vkCmdPushConstants(cmd, obj.material->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                           &obj.transform);
        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}

} // namespace vkr
