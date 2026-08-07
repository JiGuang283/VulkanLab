#include "RuntimeWorld.h"

#include "ModelAsset.h"
#include "ModelLight.h"
#include "TransformMath.h"
#include "render/MaterialInstance.h"
#include "render/Mesh.h"
#include "render/RenderCommand.h"
#include "scene/BoundsMath.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace vkr {
namespace {

bool validAtmosphereTransform(const SceneTransformDocument &transform) {
    const glm::quat rotation = glm::normalize(transform.rotation);
    return std::abs(rotation.w - 1.0f) <= 1.0e-4f &&
           std::abs(rotation.x) <= 1.0e-4f &&
           std::abs(rotation.y) <= 1.0e-4f &&
           std::abs(rotation.z) <= 1.0e-4f &&
           glm::all(glm::lessThanEqual(
               glm::abs(transform.scale - glm::vec3(1.0f)),
               glm::vec3(1.0e-4f)));
}

LightType runtimeLightType(SceneDocumentLightType type) {
    switch (type) {
    case SceneDocumentLightType::Directional:
        return LightType::Directional;
    case SceneDocumentLightType::Point:
        return LightType::Point;
    case SceneDocumentLightType::Spot:
        return LightType::Spot;
    }
    return LightType::Directional;
}

void eraseHandle(std::vector<EntityHandle> &handles, EntityHandle handle) {
    handles.erase(std::remove(handles.begin(), handles.end(), handle),
                  handles.end());
}

} // namespace

const char *modelBindingStateName(ModelBindingState state) {
    switch (state) {
    case ModelBindingState::Unresolved:
        return "Unresolved";
    case ModelBindingState::Loading:
        return "Loading";
    case ModelBindingState::Ready:
        return "Ready";
    case ModelBindingState::Failed:
        return "Failed";
    }
    return "Unknown";
}

std::unique_ptr<RuntimeWorld> RuntimeWorld::fromDocument(
    const SceneDocument &document,
    const std::vector<ResolvedModelAsset> &assets,
    const std::vector<ResolvedEnvironmentAsset> &environments) {
    SceneDocumentService::validate(document);
    auto world = std::make_unique<RuntimeWorld>();
    world->id_ = document.id;
    world->displayName_ = document.displayName;
    world->activeCamera_ = document.activeCamera;
    world->ambient_ = document.ambient;
    world->environment_ = document.environment;

    std::unordered_map<std::string, const ResolvedModelAsset *> resolved;
    for (const ResolvedModelAsset &asset : assets)
        resolved[asset.modelId.value()] = &asset;
    std::unordered_map<std::string, const ResolvedEnvironmentAsset *>
        resolvedEnvironments;
    for (const ResolvedEnvironmentAsset &environment : environments)
        resolvedEnvironments[environment.environmentId] = &environment;

    for (const SceneEntityDocument &source : document.entities) {
        SceneEntityDocument entity = source;
        entity.parent.reset();
        world->createEntity(std::move(entity));
    }
    for (const SceneEntityDocument &source : document.entities) {
        if (!source.parent)
            continue;
        const EntityHandle child = world->find(source.id);
        const EntityHandle parent = world->find(*source.parent);
        if (!world->setParent(child, parent))
            throw std::runtime_error("Could not restore scene hierarchy");
    }
    for (const SceneEntityDocument &source : document.entities) {
        RuntimeWorld::Slot *slot = world->slot(world->find(source.id));
        if (source.modelInstance) {
            const auto found =
                resolved.find(source.modelInstance->model.value());
            if (slot && slot->model && found != resolved.end() &&
                found->second->asset.asset()) {
                slot->model->profileId = found->second->profileId;
                slot->model->asset = found->second->asset;
                slot->model->state = ModelBindingState::Ready;
            }
        }
        if (source.reflectionProbe &&
            source.reflectionProbe->environmentId) {
            const auto foundEnvironment = resolvedEnvironments.find(
                *source.reflectionProbe->environmentId);
            if (slot && slot->reflectionProbe &&
                foundEnvironment != resolvedEnvironments.end() &&
                foundEnvironment->second->asset.asset()) {
                slot->reflectionProbe->profileId =
                    foundEnvironment->second->profileId;
                slot->reflectionProbe->asset =
                    foundEnvironment->second->asset;
                slot->reflectionProbe->state = ModelBindingState::Ready;
            }
        }
    }
    world->rebuildDerivedState();
    return world;
}

SceneDocument RuntimeWorld::toDocument() const {
    SceneDocument document;
    document.id = id_;
    document.displayName = displayName_;
    document.activeCamera = activeCamera_;
    document.ambient = ambient_;
    document.environment = environment_;
    document.entities.reserve(order_.size());
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry)
            continue;
        SceneEntityDocument entity;
        entity.id = entry->id;
        entity.name = entry->name;
        entity.enabled = entry->enabled;
        entity.transform = entry->transform.local;
        if (entry->parent) {
            if (const Slot *parent = slot(*entry->parent))
                entity.parent = parent->id;
        }
        if (entry->model)
            entity.modelInstance = ModelInstanceDocument{entry->model->modelId};
        entity.light = entry->light;
        entity.camera = entry->camera;
        entity.atmosphere = entry->atmosphere;
        if (entry->reflectionProbe)
            entity.reflectionProbe = entry->reflectionProbe->document;
        entity.ddgiProbeVolume = entry->ddgiProbeVolume;
        document.entities.push_back(std::move(entity));
    }
    return document;
}

std::vector<ResolvedModelAsset> RuntimeWorld::resolvedModelAssets() const {
    std::vector<ResolvedModelAsset> result;
    std::unordered_set<std::string> seen;
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->model ||
            entry->model->state != ModelBindingState::Ready ||
            !entry->model->asset.asset() ||
            !seen.insert(entry->model->modelId.value()).second) {
            continue;
        }
        result.push_back({entry->model->modelId, entry->model->profileId,
                          entry->model->asset});
    }
    return result;
}

std::vector<ResolvedEnvironmentAsset>
RuntimeWorld::resolvedEnvironmentAssets() const {
    std::vector<ResolvedEnvironmentAsset> result;
    std::unordered_set<std::string> seen;
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->reflectionProbe ||
            !entry->reflectionProbe->document.environmentId ||
            entry->reflectionProbe->state != ModelBindingState::Ready ||
            !entry->reflectionProbe->asset.asset() ||
            !seen.insert(
                     *entry->reflectionProbe->document.environmentId)
                 .second) {
            continue;
        }
        result.push_back({
            *entry->reflectionProbe->document.environmentId,
            entry->reflectionProbe->profileId,
            entry->reflectionProbe->asset});
    }
    return result;
}

void RuntimeWorld::replaceDocument(const SceneDocument &document) {
    std::unique_ptr<RuntimeWorld> replacement =
        fromDocument(document, resolvedModelAssets(),
                     resolvedEnvironmentAssets());
    id_ = std::move(replacement->id_);
    displayName_ = std::move(replacement->displayName_);
    activeCamera_ = std::move(replacement->activeCamera_);
    ambient_ = replacement->ambient_;
    environment_ = std::move(replacement->environment_);
    slots_ = std::move(replacement->slots_);
    freeList_ = std::move(replacement->freeList_);
    order_ = std::move(replacement->order_);
    byId_ = std::move(replacement->byId_);
    bounds_ = replacement->bounds_;
    lights_ = std::move(replacement->lights_);
    atmosphere_ = std::move(replacement->atmosphere_);
    reflectionProbes_ = std::move(replacement->reflectionProbes_);
    ddgiProbeVolume_ = std::move(replacement->ddgiProbeVolume_);
    materials_ = std::move(replacement->materials_);
    derivedDirty_ = true;
}

RuntimeWorld::Slot *RuntimeWorld::slot(EntityHandle handle) {
    if (!valid(handle))
        return nullptr;
    return &slots_[handle.index];
}

const RuntimeWorld::Slot *RuntimeWorld::slot(EntityHandle handle) const {
    if (!valid(handle))
        return nullptr;
    return &slots_[handle.index];
}

bool RuntimeWorld::valid(EntityHandle handle) const {
    return handle.index < slots_.size() && slots_[handle.index].alive &&
           slots_[handle.index].generation == handle.generation;
}

EntityHandle RuntimeWorld::find(const PersistentEntityId &id) const {
    const auto found = byId_.find(id);
    return found == byId_.end() ? EntityHandle{} : found->second;
}

EntityHandle RuntimeWorld::createEntity(SceneEntityDocument entity,
                                        std::optional<size_t> insertIndex) {
    if (entity.id.empty() || byId_.find(entity.id) != byId_.end())
        throw std::invalid_argument("Runtime entity UUID is invalid or duplicate");
    if (entity.atmosphere) {
        for (EntityHandle handle : order_) {
            const Slot *existing = slot(handle);
            if (existing && existing->atmosphere) {
                throw std::invalid_argument(
                    "RuntimeWorld supports only one atmosphere component");
            }
        }
        if (entity.parent || !validAtmosphereTransform(entity.transform)) {
            throw std::invalid_argument(
                "Atmosphere entity must be a root with identity rotation and unit scale");
        }
    }
    if (entity.ddgiProbeVolume) {
        for (EntityHandle handle : order_) {
            const Slot *existing = slot(handle);
            if (existing && existing->ddgiProbeVolume) {
                throw std::invalid_argument(
                    "RuntimeWorld supports only one DDGI probe volume");
            }
        }
        if (entity.parent ||
            glm::any(glm::greaterThan(
                glm::abs(entity.transform.scale - glm::vec3(1.0f)),
                glm::vec3(1.0e-4f)))) {
            throw std::invalid_argument(
                "DDGI probe volume must be a root with unit scale");
        }
    }
    uint32_t index = 0;
    if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
    } else {
        index = static_cast<uint32_t>(slots_.size());
        slots_.push_back({});
    }
    Slot &entry = slots_[index];
    entry.alive = true;
    entry.id = entity.id;
    entry.name = std::move(entity.name);
    entry.enabled = entity.enabled;
    entry.effectiveEnabled = entity.enabled;
    entry.parent.reset();
    entry.children.clear();
    entry.transform = {entity.transform, glm::mat4(1.0f),
                       glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true};
    entry.light = std::move(entity.light);
    entry.camera = std::move(entity.camera);
    entry.atmosphere = std::move(entity.atmosphere);
    entry.ddgiProbeVolume = std::move(entity.ddgiProbeVolume);
    if (entity.reflectionProbe) {
        RuntimeReflectionProbeComponent component;
        component.document = std::move(*entity.reflectionProbe);
        entry.reflectionProbe = std::move(component);
    } else {
        entry.reflectionProbe.reset();
    }
    if (entity.modelInstance) {
        RuntimeModelInstanceComponent component;
        component.modelId = entity.modelInstance->model;
        entry.model = std::move(component);
    } else {
        entry.model.reset();
    }
    const EntityHandle handle{index, entry.generation};
    byId_[entry.id] = handle;
    const size_t position = std::min(insertIndex.value_or(order_.size()),
                                     order_.size());
    order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(position),
                  handle);
    derivedDirty_ = true;
    return handle;
}

bool RuntimeWorld::destroyEntity(EntityHandle handle,
                                 bool includeDescendants) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (!includeDescendants && !entry->children.empty())
        return false;
    const auto children = entry->children;
    for (EntityHandle child : children)
        destroyEntity(child, true);
    destroyOne(handle);
    derivedDirty_ = true;
    return true;
}

void RuntimeWorld::destroyOne(EntityHandle handle) {
    Slot *entry = slot(handle);
    if (!entry)
        return;
    if (entry->parent) {
        if (Slot *parent = slot(*entry->parent)) {
            eraseHandle(parent->children, handle);
        }
    }
    if (activeCamera_ && *activeCamera_ == entry->id)
        activeCamera_.reset();
    byId_.erase(entry->id);
    eraseHandle(order_, handle);
    entry->model.reset();
    entry->reflectionProbe.reset();
    entry->ddgiProbeVolume.reset();
    entry->children.clear();
    entry->alive = false;
    ++entry->generation;
    freeList_.push_back(handle.index);
}

bool RuntimeWorld::wouldCreateCycle(EntityHandle handle,
                                    EntityHandle parent) const {
    EntityHandle current = parent;
    while (current) {
        if (current == handle)
            return true;
        const Slot *entry = slot(current);
        if (!entry || !entry->parent)
            break;
        current = *entry->parent;
    }
    return false;
}

bool RuntimeWorld::setParent(EntityHandle handle,
                             std::optional<EntityHandle> parent,
                             ReparentMode mode, std::string *error) {
    if (error)
        error->clear();
    Slot *entry = slot(handle);
    if (!entry || (parent && !valid(*parent))) {
        if (error)
            *error = "invalid_parent";
        return false;
    }
    if ((entry->atmosphere || entry->ddgiProbeVolume) && parent) {
        if (error)
            *error = entry->atmosphere ? "atmosphere_must_be_root"
                                       : "ddgi_volume_must_be_root";
        return false;
    }
    if (parent && wouldCreateCycle(handle, *parent)) {
        if (error)
            *error = "parent_cycle";
        return false;
    }
    if (entry->parent == parent)
        return true;

    SceneTransformDocument localAfterReparent = entry->transform.local;
    if (mode == ReparentMode::KeepWorld) {
        rebuildDerivedState();
        const glm::mat4 oldWorld = entry->transform.world;
        glm::mat4 newLocal = oldWorld;
        if (parent) {
            const Slot *newParent = slot(*parent);
            const float determinant = glm::determinant(newParent->transform.world);
            if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f) {
                if (error)
                    *error = "transform_not_decomposable";
                return false;
            }
            newLocal = glm::inverse(newParent->transform.world) * oldWorld;
        }
        if (!decomposeSceneTransform(newLocal, localAfterReparent)) {
            if (error)
                *error = "transform_not_decomposable";
            return false;
        }
    }

    if (entry->parent) {
        if (Slot *oldParent = slot(*entry->parent))
            eraseHandle(oldParent->children, handle);
    }
    entry->parent = parent;
    entry->transform.local = localAfterReparent;
    if (parent)
        slot(*parent)->children.push_back(handle);
    markDirty(handle);
    return true;
}

bool RuntimeWorld::setName(EntityHandle handle, std::string name) {
    Slot *entry = slot(handle);
    if (!entry || name.empty())
        return false;
    entry->name = std::move(name);
    return true;
}

bool RuntimeWorld::setEnabled(EntityHandle handle, bool enabled) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    entry->enabled = enabled;
    markDirty(handle);
    return true;
}

bool RuntimeWorld::setTransform(EntityHandle handle,
                                const SceneTransformDocument &transform) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (entry->atmosphere && !validAtmosphereTransform(transform))
        return false;
    if (entry->ddgiProbeVolume &&
        glm::any(glm::greaterThan(
            glm::abs(transform.scale - glm::vec3(1.0f)),
            glm::vec3(1.0e-4f))))
        return false;
    entry->transform.local = transform;
    entry->transform.local.rotation =
        glm::normalize(entry->transform.local.rotation);
    markDirty(handle);
    return true;
}

bool RuntimeWorld::setModelInstance(
    EntityHandle handle, std::optional<ModelInstanceDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (!component) {
        entry->model.reset();
    } else {
        RuntimeModelInstanceComponent runtime;
        runtime.modelId = component->model;
        if (entry->model)
            runtime.bindingRevision = entry->model->bindingRevision + 1;
        entry->model = std::move(runtime);
    }
    derivedDirty_ = true;
    return true;
}

bool RuntimeWorld::setLight(
    EntityHandle handle, std::optional<LightComponentDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    entry->light = std::move(component);
    derivedDirty_ = true;
    return true;
}

bool RuntimeWorld::setCamera(
    EntityHandle handle, std::optional<CameraComponentDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (!component && activeCamera_ && *activeCamera_ == entry->id)
        return false;
    entry->camera = std::move(component);
    return true;
}

bool RuntimeWorld::setAtmosphere(
    EntityHandle handle,
    std::optional<AtmosphereComponentDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (component) {
        if (entry->parent || !validAtmosphereTransform(entry->transform.local))
            return false;
        for (EntityHandle candidate : order_) {
            const Slot *other = slot(candidate);
            if (other && other != entry && other->atmosphere)
                return false;
        }
    }
    entry->atmosphere = std::move(component);
    derivedDirty_ = true;
    return true;
}

bool RuntimeWorld::setReflectionProbe(
    EntityHandle handle,
    std::optional<ReflectionProbeComponentDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (!component) {
        entry->reflectionProbe.reset();
    } else {
        if (entry->reflectionProbe &&
            entry->reflectionProbe->document.environmentId ==
                component->environmentId) {
            entry->reflectionProbe->document = std::move(*component);
            derivedDirty_ = true;
            return true;
        }
        RuntimeReflectionProbeComponent runtime;
        runtime.document = std::move(*component);
        if (entry->reflectionProbe) {
            runtime.bindingRevision =
                entry->reflectionProbe->bindingRevision + 1;
        }
        entry->reflectionProbe = std::move(runtime);
    }
    derivedDirty_ = true;
    return true;
}

bool RuntimeWorld::setDdgiProbeVolume(
    EntityHandle handle,
    std::optional<DdgiProbeVolumeComponentDocument> component) {
    Slot *entry = slot(handle);
    if (!entry)
        return false;
    if (component) {
        if (entry->parent ||
            glm::any(glm::greaterThan(
                glm::abs(entry->transform.local.scale - glm::vec3(1.0f)),
                glm::vec3(1.0e-4f)))) {
            return false;
        }
        for (EntityHandle candidate : order_) {
            const Slot *other = slot(candidate);
            if (other && other != entry && other->ddgiProbeVolume)
                return false;
        }
    }
    entry->ddgiProbeVolume = std::move(component);
    derivedDirty_ = true;
    return true;
}

bool RuntimeWorld::setActiveCamera(const PersistentEntityId &id) {
    Slot *entry = slot(find(id));
    if (!entry || !entry->camera)
        return false;
    activeCamera_ = id;
    return true;
}

bool RuntimeWorld::bindModel(EntityHandle handle, uint64_t expectedRevision,
                             std::string profileId, ModelAssetHandle asset,
                             std::string error) {
    Slot *entry = slot(handle);
    if (!entry || !entry->model ||
        entry->model->bindingRevision != expectedRevision)
        return false;
    entry->model->profileId = std::move(profileId);
    entry->model->asset = std::move(asset);
    entry->model->error = std::move(error);
    entry->model->state = !entry->model->error.empty()
                              ? ModelBindingState::Failed
                              : (entry->model->asset.asset()
                                     ? ModelBindingState::Ready
                                     : ModelBindingState::Loading);
    derivedDirty_ = true;
    return true;
}

uint64_t RuntimeWorld::modelBindingRevision(EntityHandle handle) const {
    const Slot *entry = slot(handle);
    return entry && entry->model ? entry->model->bindingRevision : 0;
}

std::shared_ptr<const ModelAsset>
RuntimeWorld::modelAsset(EntityHandle handle) const {
    const Slot *entry = slot(handle);
    return entry && entry->model ? entry->model->asset.asset() : nullptr;
}

bool RuntimeWorld::bindReflectionProbe(
    EntityHandle handle, uint64_t expectedRevision,
    std::string profileId, EnvironmentAssetHandle asset,
    std::string error) {
    Slot *entry = slot(handle);
    if (!entry || !entry->reflectionProbe ||
        entry->reflectionProbe->bindingRevision != expectedRevision)
        return false;
    entry->reflectionProbe->profileId = std::move(profileId);
    entry->reflectionProbe->asset = std::move(asset);
    entry->reflectionProbe->error = std::move(error);
    entry->reflectionProbe->state =
        !entry->reflectionProbe->error.empty()
            ? ModelBindingState::Failed
            : (entry->reflectionProbe->asset.asset()
                   ? ModelBindingState::Ready
                   : ModelBindingState::Loading);
    derivedDirty_ = true;
    return true;
}

uint64_t RuntimeWorld::reflectionProbeBindingRevision(
    EntityHandle handle) const {
    const Slot *entry = slot(handle);
    return entry && entry->reflectionProbe
               ? entry->reflectionProbe->bindingRevision
               : 0;
}

void RuntimeWorld::setIdentity(SceneDocumentId id, std::string displayName) {
    id_ = std::move(id);
    displayName_ = std::move(displayName);
}

void RuntimeWorld::setAmbient(const SceneAmbientDocument &ambient) {
    ambient_ = ambient;
}

void RuntimeWorld::setEnvironment(
    std::optional<SceneEnvironmentDocument> environment) {
    environment_ = std::move(environment);
}

void RuntimeWorld::markDirty(EntityHandle handle) {
    Slot *entry = slot(handle);
    if (!entry)
        return;
    entry->transform.dirty = true;
    for (EntityHandle child : entry->children)
        markDirty(child);
    derivedDirty_ = true;
}

void RuntimeWorld::updateHierarchy(EntityHandle handle,
                                   const glm::mat4 &parentWorld,
                                   const glm::quat &parentRotation,
                                   bool parentEnabled) {
    Slot *entry = slot(handle);
    if (!entry)
        return;
    entry->transform.world =
        parentWorld * composeSceneTransform(entry->transform.local);
    entry->transform.worldRotation = glm::normalize(
        parentRotation * entry->transform.local.rotation);
    entry->transform.dirty = false;
    entry->effectiveEnabled = parentEnabled && entry->enabled;
    for (EntityHandle child : entry->children)
        updateHierarchy(child, entry->transform.world,
                        entry->transform.worldRotation,
                        entry->effectiveEnabled);
}

void RuntimeWorld::rebuildDerivedState() {
    if (!derivedDirty_)
        return;
    for (EntityHandle handle : order_) {
        Slot *entry = slot(handle);
        if (entry && !entry->parent)
            updateHierarchy(handle, glm::mat4(1.0f),
                            glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true);
    }

    bounds_ = {};
    lights_.clear();
    atmosphere_.reset();
    reflectionProbes_.clear();
    ddgiProbeVolume_.reset();
    materials_.clear();
    std::unordered_set<const MaterialInstance *> seenMaterials;

    // Explicit scene lights have deterministic priority over imported lights.
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled || !entry->light)
            continue;
        const LightComponentDocument &source = *entry->light;
        SceneLight light;
        light.debugName = entry->name;
        light.stableKey = "entity/" + entry->id.toString();
        light.source = SceneLightSource::ExplicitEntity;
        light.ownerEntity = entry->id;
        light.castsShadow = source.castsShadow;
        light.atmosphereSunIndex = source.atmosphereSunIndex;
        light.sourceAngularRadiusRadians =
            source.sourceAngularRadiusRadians;
        light.type = runtimeLightType(source.type);
        light.positionWS = glm::vec3(entry->transform.world[3]);
        light.color = source.color;
        light.intensity = source.intensity;
        light.range = source.range.value_or(0.0f);
        light.innerConeCos = std::cos(source.innerConeRadians);
        light.outerConeCos = std::cos(source.outerConeRadians);
        if (light.type == LightType::Directional) {
            light.directionWS = glm::normalize(
                entry->transform.worldRotation *
                glm::vec3(0.0f, 0.0f, 1.0f));
        } else if (light.type == LightType::Spot) {
            light.directionWS = glm::normalize(
                entry->transform.worldRotation *
                glm::vec3(0.0f, 0.0f, -1.0f));
        }
        lights_.push_back(std::move(light));
    }

    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled ||
            !entry->reflectionProbe ||
            entry->reflectionProbe->state != ModelBindingState::Ready)
            continue;
        std::shared_ptr<EnvironmentGpuResources> environment =
            entry->reflectionProbe->asset.asset();
        if (!environment)
            continue;
        RenderWorldReflectionProbe probe;
        probe.entityId = entry->id;
        probe.world = entry->transform.world;
        probe.worldToLocal = glm::inverse(entry->transform.world);
        probe.capturePositionWS =
            glm::vec3(entry->transform.world *
                      glm::vec4(entry->reflectionProbe->document.captureOffset,
                                1.0f));
        probe.parameters = entry->reflectionProbe->document;
        probe.environment = std::move(environment);
        probe.environmentGeneration =
            entry->reflectionProbe->asset.generation();
        reflectionProbes_.push_back(std::move(probe));
    }

    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled ||
            !entry->ddgiProbeVolume)
            continue;
        ddgiProbeVolume_ = RenderWorldDdgiVolume{
            entry->id, entry->transform.world,
            glm::inverse(entry->transform.world),
            *entry->ddgiProbeVolume};
        break;
    }

    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled || !entry->atmosphere)
            continue;
        atmosphere_ = RenderWorldAtmosphere{
            entry->id, glm::vec3(entry->transform.world[3]),
            *entry->atmosphere};
        break;
    }

    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled || !entry->model ||
            entry->model->state != ModelBindingState::Ready)
            continue;
        const std::shared_ptr<const ModelAsset> asset =
            entry->model->asset.asset();
        if (!asset)
            continue;
        includeTransformedBounds(bounds_, asset->localBounds,
                                 entry->transform.world);
        for (const auto &material : asset->materials) {
            if (material && seenMaterials.insert(material.get()).second)
                materials_.push_back(material);
        }
        for (size_t lightIndex = 0; lightIndex < asset->lights.size();
             ++lightIndex) {
            const std::string stableKey =
                "model/" + entry->id.toString() + "/" +
                std::to_string(lightIndex);
            lights_.push_back(instantiateModelLight(
                asset->lights[lightIndex], entry->transform.world,
                stableKey, entry->id));
        }
    }
    derivedDirty_ = false;
}

void RuntimeWorld::refreshModelBindingStates() {
    for (EntityHandle handle : order_) {
        Slot *entry = slot(handle);
        if (!entry || !entry->model ||
            entry->model->state != ModelBindingState::Loading)
            continue;
        const ModelAssetHandleSnapshot snapshot = entry->model->asset.snapshot();
        if (snapshot.state == ModelAssetState::Ready ||
            snapshot.state == ModelAssetState::Retiring) {
            entry->model->state = ModelBindingState::Ready;
            entry->model->error.clear();
            derivedDirty_ = true;
        } else if (snapshot.state == ModelAssetState::Failed ||
                   snapshot.state == ModelAssetState::Cancelled) {
            entry->model->state = ModelBindingState::Failed;
            entry->model->error = snapshot.error.empty()
                                      ? "Model asset loading failed"
                                      : snapshot.error;
            derivedDirty_ = true;
        }
    }
}

void RuntimeWorld::refreshReflectionProbeBindingStates() {
    for (EntityHandle handle : order_) {
        Slot *entry = slot(handle);
        if (!entry || !entry->reflectionProbe ||
            entry->reflectionProbe->state != ModelBindingState::Loading)
            continue;
        const EnvironmentAssetHandleSnapshot snapshot =
            entry->reflectionProbe->asset.snapshot();
        if (snapshot.state == EnvironmentAssetState::Ready ||
            snapshot.state == EnvironmentAssetState::Retiring) {
            entry->reflectionProbe->state = ModelBindingState::Ready;
            entry->reflectionProbe->error.clear();
            derivedDirty_ = true;
        } else if (snapshot.state == EnvironmentAssetState::Failed ||
                   snapshot.state == EnvironmentAssetState::Cancelled) {
            entry->reflectionProbe->state = ModelBindingState::Failed;
            entry->reflectionProbe->error =
                snapshot.error.empty()
                    ? "Reflection probe environment loading failed"
                    : snapshot.error;
            derivedDirty_ = true;
        }
    }
}

void RuntimeWorld::update(float, float) {
    refreshModelBindingStates();
    refreshReflectionProbeBindingStates();
    rebuildDerivedState();
}

void RuntimeWorld::collectRenderItems(std::vector<RenderItem> &items) const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled || !entry->model ||
            entry->model->state != ModelBindingState::Ready)
            continue;
        const std::shared_ptr<const ModelAsset> asset =
            entry->model->asset.asset();
        if (!asset)
            continue;
        uint32_t primitiveIndex = 0;
        for (const ModelPrimitive &primitive : asset->primitives) {
            const MaterialInstance *material = primitive.material.get();
            const MaterialParams *params =
                material ? &material->params() : nullptr;
            const bool transparent =
                params && (params->alphaMode == AlphaMode::Blend ||
                           params->transmissionFactor > 0.0f);
            RenderItem item{};
            item.mesh = primitive.mesh.get();
            item.material = material;
            item.world =
                entry->transform.world * primitive.localToAsset;
            item.queue = transparent ? RenderQueueType::Transparent
                                     : RenderQueueType::Opaque;
            if (item.mesh)
                item.localBounds = item.mesh->localBounds();
            item.primitiveIndex = primitiveIndex;
            item.key.ownerKind = RenderItemOwnerKind::NativeEntity;
            item.key.entityId = entry->id;
            item.key.assetGeneration = asset->generation;
            item.key.primitiveIndex = primitiveIndex;
            item.sourceOrder = static_cast<uint32_t>(items.size());
            items.push_back(std::move(item));
            ++primitiveIndex;
        }
    }
}

size_t RuntimeWorld::renderableCount() const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    size_t result = 0;
    for (EntityHandle handle : order_) {
        const Slot *entry = slot(handle);
        if (!entry || !entry->effectiveEnabled || !entry->model ||
            entry->model->state != ModelBindingState::Ready)
            continue;
        if (const auto asset = entry->model->asset.asset())
            result += asset->primitives.size();
    }
    return result;
}

std::optional<RuntimeCameraView>
RuntimeWorld::activeCamera(float aspect) const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    if (!activeCamera_)
        return std::nullopt;
    const Slot *entry = slot(find(*activeCamera_));
    if (!entry || !entry->camera)
        return std::nullopt;
    const glm::vec3 position = glm::vec3(entry->transform.world[3]);
    const glm::vec3 forward = glm::normalize(
        entry->transform.worldRotation * glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 up = glm::normalize(
        entry->transform.worldRotation * glm::vec3(0.0f, 1.0f, 0.0f));
    RuntimeCameraView result;
    result.position = position;
    result.nearPlane = entry->camera->nearPlane;
    result.farPlane = entry->camera->farPlane;
    result.entityId = entry->id;
    result.view = glm::lookAt(position, position + forward, up);
    result.projection = glm::perspective(
        entry->camera->verticalFovRadians, std::max(aspect, 0.001f),
        entry->camera->nearPlane, entry->camera->farPlane);
    result.projection[1][1] *= -1.0f;
    return result;
}

std::optional<RenderWorldAmbient> RuntimeWorld::worldAmbient() const {
    return RenderWorldAmbient{ambient_.color, ambient_.intensity};
}

std::optional<RenderWorldEnvironment> RuntimeWorld::worldEnvironment() const {
    if (!environment_)
        return std::nullopt;
    return RenderWorldEnvironment{environment_->environmentId,
                                  environment_->intensity,
                                  environment_->rotationRadians};
}

std::optional<RenderWorldAtmosphere> RuntimeWorld::worldAtmosphere() const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    return atmosphere_;
}

const std::vector<RenderWorldReflectionProbe> &
RuntimeWorld::reflectionProbes() const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    return reflectionProbes_;
}

std::optional<RenderWorldDdgiVolume>
RuntimeWorld::ddgiProbeVolume() const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    return ddgiProbeVolume_;
}

std::vector<RuntimeEntitySnapshot> RuntimeWorld::entities() const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    std::vector<RuntimeEntitySnapshot> result;
    result.reserve(order_.size());
    for (EntityHandle handle : order_) {
        if (auto value = entity(handle))
            result.push_back(std::move(*value));
    }
    return result;
}

std::optional<RuntimeEntitySnapshot>
RuntimeWorld::entity(EntityHandle handle) const {
    const_cast<RuntimeWorld *>(this)->rebuildDerivedState();
    const Slot *entry = slot(handle);
    if (!entry)
        return std::nullopt;
    RuntimeEntitySnapshot result;
    result.handle = handle;
    result.id = entry->id;
    result.name = entry->name;
    result.enabled = entry->enabled;
    result.effectiveEnabled = entry->effectiveEnabled;
    result.transform = entry->transform.local;
    result.world = entry->transform.world;
    if (entry->parent) {
        if (const Slot *parent = slot(*entry->parent))
            result.parent = parent->id;
    }
    if (entry->model) {
        result.modelInstance = ModelInstanceDocument{entry->model->modelId};
        result.modelBindingState = entry->model->state;
        result.modelBindingError = entry->model->error;
        result.modelProfileId = entry->model->profileId;
        result.modelBindingRevision = entry->model->bindingRevision;
    }
    result.light = entry->light;
    result.camera = entry->camera;
    result.atmosphere = entry->atmosphere;
    if (entry->reflectionProbe) {
        result.reflectionProbe = entry->reflectionProbe->document;
        result.reflectionProbeBindingState =
            entry->reflectionProbe->state;
        result.reflectionProbeBindingError =
            entry->reflectionProbe->error;
        result.reflectionProbeProfileId =
            entry->reflectionProbe->profileId;
        result.reflectionProbeBindingRevision =
            entry->reflectionProbe->bindingRevision;
    }
    result.ddgiProbeVolume = entry->ddgiProbeVolume;
    return result;
}

size_t RuntimeWorld::modelInstanceCount() const {
    return static_cast<size_t>(std::count_if(
        order_.begin(), order_.end(), [this](EntityHandle handle) {
            const Slot *entry = slot(handle);
            return entry && entry->model.has_value();
        }));
}

size_t RuntimeWorld::explicitLightCount() const {
    return static_cast<size_t>(std::count_if(
        order_.begin(), order_.end(), [this](EntityHandle handle) {
            const Slot *entry = slot(handle);
            return entry && entry->light.has_value();
        }));
}

} // namespace vkr
