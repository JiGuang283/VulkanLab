#include "Scene.h"
#include "render/MaterialInstance.h"
#include "render/Mesh.h"
#include "render/RenderQueue.h"

#include <algorithm>
#include <utility>

namespace vkr {

namespace {

void includePoint(Bounds &bounds, const glm::vec3 &point) {
    if (!bounds.valid) {
        bounds.min = point;
        bounds.max = point;
        bounds.center = point;
        bounds.radius = 0.0f;
        bounds.valid = true;
        return;
    }

    bounds.min = glm::min(bounds.min, point);
    bounds.max = glm::max(bounds.max, point);
    bounds.center = (bounds.min + bounds.max) * 0.5f;
    bounds.radius = glm::length(bounds.max - bounds.center);
}

void includeTransformedBounds(Bounds &sceneBounds, const Bounds &localBounds,
                              const glm::mat4 &transform) {
    if (!localBounds.valid)
        return;

    const glm::vec3 minV = localBounds.min;
    const glm::vec3 maxV = localBounds.max;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                const glm::vec3 corner{x ? maxV.x : minV.x,
                                       y ? maxV.y : minV.y,
                                       z ? maxV.z : minV.z};
                includePoint(sceneBounds,
                             glm::vec3(transform * glm::vec4(corner, 1.0f)));
            }
        }
    }
}

} // namespace

void Scene::addObject(SceneObject obj) {
    if (obj.mesh)
        includeTransformedBounds(bounds_, obj.mesh->localBounds(),
                                 obj.transform);
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
