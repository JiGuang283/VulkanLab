#include "RenderQueue.h"

#include "MaterialInstance.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <unordered_set>

namespace vkr {

void RenderQueue::clear() {
    opaque_.clear();
    transparent_.clear();
}

void RenderQueue::add(RenderCommand command) {
    switch (command.queue) {
    case RenderQueueType::Opaque:
        opaque_.push_back(command);
        break;
    case RenderQueueType::Transparent:
        transparent_.push_back(command);
        break;
    }
}

void RenderQueue::sortOpaque() {
    std::stable_sort(opaque_.begin(), opaque_.end(),
                     [](const RenderCommand &a, const RenderCommand &b) {
                         const auto *at = a.material
                                              ? &a.material->materialTemplate()
                                              : nullptr;
                         const auto *bt = b.material
                                              ? &b.material->materialTemplate()
                                              : nullptr;
                         if (at != bt)
                             return at < bt;
                         if (a.material != b.material)
                             return a.material < b.material;
                         return a.mesh < b.mesh;
                     });
}

void RenderQueue::sortTransparent(const glm::vec3 &cameraPosition) {
    std::stable_sort(transparent_.begin(), transparent_.end(),
                     [&cameraPosition](const RenderCommand &a,
                                       const RenderCommand &b) {
                         const glm::vec3 apos(a.world[3]);
                         const glm::vec3 bpos(b.world[3]);
                         const float     ad = glm::dot(apos - cameraPosition,
                                                   apos - cameraPosition);
                         const float     bd = glm::dot(bpos - cameraPosition,
                                                   bpos - cameraPosition);
                         return ad > bd;
                     });
}

size_t RenderQueue::uniqueMaterialCount() const {
    std::unordered_set<const MaterialInstance *> materials;
    for (const auto &command : opaque_)
        materials.insert(command.material);
    for (const auto &command : transparent_)
        materials.insert(command.material);
    return materials.size();
}

size_t RenderQueue::uniqueMeshCount() const {
    std::unordered_set<const Mesh *> meshes;
    for (const auto &command : opaque_)
        meshes.insert(command.mesh);
    for (const auto &command : transparent_)
        meshes.insert(command.mesh);
    return meshes.size();
}

} // namespace vkr

