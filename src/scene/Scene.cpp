#include "Scene.h"
#include "ModelAsset.h"
#include "ModelLight.h"
#include "render/MaterialInstance.h"
#include "render/Mesh.h"
#include "render/RenderCommand.h"
#include "scene/BoundsMath.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace vkr {

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

    for (size_t instanceIndex = 0; instanceIndex < modelInstances_.size();
         ++instanceIndex) {
        const ModelInstance &instance = modelInstances_[instanceIndex];
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
        for (size_t lightIndex = 0; lightIndex < asset->lights.size();
             ++lightIndex) {
            const std::string stableKey =
                "model-preview/" + asset->id.value() + "/" +
                std::to_string(instanceIndex) + "/" +
                std::to_string(lightIndex);
            lights_.push_back(instantiateModelLight(
                asset->lights[lightIndex], instance.rootToWorld, stableKey));
        }
    }
}

void Scene::collectRenderItems(std::vector<RenderItem> &items) const {
    uint32_t sourceIndex = 0;
    for (const auto &obj : objects_) {
        const auto *material = obj.material.get();
        const auto *params = material ? &material->params() : nullptr;
        const bool transparent =
            params && (params->alphaMode == AlphaMode::Blend ||
                       params->transmissionFactor > 0.0f);
        const RenderQueueType queueType = transparent
                                              ? RenderQueueType::Transparent
                                              : RenderQueueType::Opaque;
        RenderItem item{};
        item.mesh = obj.mesh.get();
        item.material = obj.material.get();
        item.world = obj.transform;
        item.queue = queueType;
        if (item.mesh)
            item.localBounds = item.mesh->localBounds();
        item.key.ownerKind = RenderItemOwnerKind::LegacyObject;
        item.key.fallbackOrdinal = sourceIndex + 1u;
        item.sourceOrder = sourceIndex;
        items.push_back(std::move(item));
        ++sourceIndex;
    }

    uint32_t instanceIndex = 0;
    for (const ModelInstance &instance : modelInstances_) {
        if (!instance.visible) {
            ++instanceIndex;
            continue;
        }
        const auto asset = instance.asset.asset();
        if (!asset) {
            ++instanceIndex;
            continue;
        }
        uint32_t primitiveIndex = 0;
        for (const ModelPrimitive &primitive : asset->primitives) {
            const auto *material = primitive.material.get();
            const auto *params = material ? &material->params() : nullptr;
            const bool transparent =
                params && (params->alphaMode == AlphaMode::Blend ||
                           params->transmissionFactor > 0.0f);
            RenderItem item{};
            item.mesh = primitive.mesh.get();
            item.material = material;
            item.world = instance.rootToWorld * primitive.localToAsset;
            item.queue = transparent ? RenderQueueType::Transparent
                                     : RenderQueueType::Opaque;
            if (item.mesh)
                item.localBounds = item.mesh->localBounds();
            item.primitiveIndex = primitiveIndex;
            item.key.ownerKind = RenderItemOwnerKind::PreviewInstance;
            item.key.assetGeneration = asset->generation;
            item.key.primitiveIndex = primitiveIndex;
            item.key.fallbackOrdinal = instanceIndex + 1u;
            item.sourceOrder = sourceIndex++;
            items.push_back(std::move(item));
            ++primitiveIndex;
        }
        ++instanceIndex;
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
