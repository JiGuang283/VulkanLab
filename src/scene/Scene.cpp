#include "Scene.h"
#include "render/RenderQueue.h"

#include <stdexcept>
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

const MaterialTemplate &Scene::primaryMaterialTemplate() const {
    if (materialTemplates_.empty())
        throw std::runtime_error("Scene has no material template.");
    return *materialTemplates_.front();
}

} // namespace vkr
