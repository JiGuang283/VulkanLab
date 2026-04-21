#include "Scene.h"
#include "core/Pipeline.h"
#include "render/Material.h"
#include "render/Mesh.h"

#include <glm/glm.hpp>

namespace vkr {

namespace {
struct GpuPushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveMetallic; // xyz=emissive, w=metallic
    glm::vec4 roughnessAlpha;   // x=roughness, y=alphaCutoff
    glm::vec4 reserved;         // padding to reach 128B (Vulkan min)
};
static_assert(sizeof(GpuPushBlock) == 128, "push block must be 128B");
} // namespace

void Scene::addObject(SceneObject obj) {
    objects_.push_back(std::move(obj));
}

void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex,
                   Pipeline &pipeline) const {
    // All objects currently share the same pipeline — bind once.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    for (const auto &obj : objects_) {
        obj.material->bindDescriptors(cmd, pipeline.layout(), frameIndex);

        const auto  &p = obj.material->params();
        GpuPushBlock blk{};
        blk.model = obj.transform;
        blk.baseColorFactor = p.baseColorFactor;
        blk.emissiveMetallic =
            glm::vec4(p.emissiveFactor, p.metallicFactor);
        blk.roughnessAlpha =
            glm::vec4(p.roughnessFactor, p.alphaCutoff, 0.0f, 0.0f);

        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GpuPushBlock), &blk);
        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}

} // namespace vkr
