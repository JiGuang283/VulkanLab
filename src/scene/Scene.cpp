#include "Scene.h"
#include "render/MaterialInstance.h"
#include "render/RenderQueue.h"

#include <utility>

namespace vkr {

void Scene::addObject(SceneObject obj) {
    objects_.push_back(std::move(obj));
}

void Scene::collectRenderCommands(RenderQueue &queue) const {
    for (const auto &obj : objects_) {
        const auto *material = obj.material.get();
        const auto *params = material ? &material->params() : nullptr;
        const bool transparent =
            params && (params->alphaMode == AlphaMode::Blend ||
                       params->transmissionFactor > 0.0f);
        const RenderQueueType queueType = transparent
                                              ? RenderQueueType::Transparent
                                              : RenderQueueType::Opaque;
        queue.add(RenderCommand{
            obj.mesh.get(),
            obj.material.get(),
            obj.transform,
            queueType,
        });
    }
}

} // namespace vkr
