#include "Scene.h"
#include "render/RenderQueue.h"

#include <utility>

namespace vkr {

void Scene::addObject(SceneObject obj) {
    objects_.push_back(std::move(obj));
}

void Scene::collectRenderCommands(RenderQueue &queue) const {
    for (const auto &obj : objects_) {
        queue.add(RenderCommand{
            obj.mesh.get(),
            obj.material.get(),
            obj.transform,
            RenderQueueType::Opaque,
        });
    }
}

} // namespace vkr
