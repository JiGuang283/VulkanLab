#include "RenderQueue.h"

#include <algorithm>
#include <unordered_set>

namespace vkr {

void RenderQueue::clear() {
    opaque_.clear();
}

void RenderQueue::add(RenderCommand command) {
    switch (command.queue) {
    case RenderQueueType::Opaque:
    case RenderQueueType::Transparent:
        // Transparent is intentionally routed through opaque in this
        // transition step. A dedicated transparent queue comes later.
        opaque_.push_back(command);
        break;
    }
}

void RenderQueue::sortOpaque() {
    std::stable_sort(opaque_.begin(), opaque_.end(),
                     [](const RenderCommand &a, const RenderCommand &b) {
                         if (a.material != b.material)
                             return a.material < b.material;
                         return a.mesh < b.mesh;
                     });
}

size_t RenderQueue::uniqueMaterialCount() const {
    std::unordered_set<const Material *> materials;
    for (const auto &command : opaque_)
        materials.insert(command.material);
    return materials.size();
}

size_t RenderQueue::uniqueMeshCount() const {
    std::unordered_set<const Mesh *> meshes;
    for (const auto &command : opaque_)
        meshes.insert(command.mesh);
    return meshes.size();
}

} // namespace vkr

