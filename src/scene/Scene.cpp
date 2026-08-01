#include "Scene.h"
#include "ModelAsset.h"
#include "ModelLight.h"
#include "render/MaterialInstance.h"
#include "render/Mesh.h"
#include "render/RenderQueue.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
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
    objects_.push_back(std::move(obj));
    rebuildDerivedState();
}

size_t Scene::addModelInstance(ModelInstance instance) {
    if (!instance.asset || !instance.asset.asset())
        throw std::invalid_argument(
            "Scene model instances require a ready ModelAsset");
    modelInstances_.push_back(std::move(instance));
    rebuildDerivedState();
    return modelInstances_.size() - 1;
}

bool Scene::removeModelInstance(size_t index) {
    if (index >= modelInstances_.size())
        return false;
    modelInstances_.erase(modelInstances_.begin() +
                          static_cast<std::ptrdiff_t>(index));
    rebuildDerivedState();
    return true;
}

void Scene::rebuildDerivedState() {
    bounds_ = {};
    materials_ = legacyMaterials_;
    lights_ = legacyLights_;

    std::unordered_set<const MaterialInstance *> seenMaterials;
    for (const auto &material : materials_)
        seenMaterials.insert(material.get());

    for (const SceneObject &object : objects_) {
        if (object.mesh)
            includeTransformedBounds(bounds_, object.mesh->localBounds(),
                                     object.transform);
    }

    for (const ModelInstance &instance : modelInstances_) {
        if (!instance.visible)
            continue;
        const auto asset = instance.asset.asset();
        if (!asset)
            continue;
        includeTransformedBounds(bounds_, asset->localBounds,
                                 instance.rootToWorld);
        for (const auto &material : asset->materials) {
            if (material && seenMaterials.insert(material.get()).second)
                materials_.push_back(material);
        }
        for (const ModelLightPrototype &light : asset->lights)
            lights_.push_back(
                instantiateModelLight(light, instance.rootToWorld));
    }
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

    for (const ModelInstance &instance : modelInstances_) {
        if (!instance.visible)
            continue;
        const auto asset = instance.asset.asset();
        if (!asset)
            continue;
        for (const ModelPrimitive &primitive : asset->primitives) {
            const auto *material = primitive.material.get();
            const auto *params = material ? &material->params() : nullptr;
            const bool transparent =
                params && (params->alphaMode == AlphaMode::Blend ||
                           params->transmissionFactor > 0.0f);
            queue.add(RenderCommand{
                primitive.mesh.get(),
                material,
                instance.rootToWorld * primitive.localToAsset,
                transparent ? RenderQueueType::Transparent
                            : RenderQueueType::Opaque,
            });
        }
    }
}

size_t Scene::renderableCount() const {
    size_t count = objects_.size();
    for (const ModelInstance &instance : modelInstances_) {
        if (instance.visible) {
            if (const auto asset = instance.asset.asset())
                count += asset->primitives.size();
        }
    }
    return count;
}

} // namespace vkr
