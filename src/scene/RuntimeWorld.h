#pragma once

#include "IRenderWorld.h"
#include "ModelAssetHandle.h"
#include "scene_data/SceneDocument.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

struct ModelAsset;

struct EntityHandle {
    static constexpr uint32_t kInvalidIndex =
        std::numeric_limits<uint32_t>::max();

    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;

    explicit operator bool() const { return index != kInvalidIndex; }
    friend bool operator==(EntityHandle left, EntityHandle right) {
        return left.index == right.index &&
               left.generation == right.generation;
    }
};

enum class ModelBindingState {
    Unresolved,
    Loading,
    Ready,
    Failed,
};

enum class ReparentMode {
    KeepLocal,
    KeepWorld,
};

struct RuntimeModelInstanceComponent {
    ModelAssetId modelId;
    std::string profileId;
    ModelAssetHandle asset;
    ModelBindingState state = ModelBindingState::Unresolved;
    uint64_t bindingRevision = 1;
    std::string error;
};

struct RuntimeTransformComponent {
    SceneTransformDocument local;
    glm::mat4 world{1.0f};
    glm::quat worldRotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool dirty = true;
};

struct RuntimeEntitySnapshot {
    EntityHandle handle;
    PersistentEntityId id;
    std::string name;
    std::optional<PersistentEntityId> parent;
    bool enabled = true;
    bool effectiveEnabled = true;
    SceneTransformDocument transform;
    glm::mat4 world{1.0f};
    std::optional<ModelInstanceDocument> modelInstance;
    std::optional<LightComponentDocument> light;
    std::optional<CameraComponentDocument> camera;
    std::optional<AtmosphereComponentDocument> atmosphere;
    ModelBindingState modelBindingState = ModelBindingState::Unresolved;
    std::string modelBindingError;
    std::string modelProfileId;
    uint64_t modelBindingRevision = 0;
};

struct ResolvedModelAsset {
    ModelAssetId modelId;
    std::string profileId;
    ModelAssetHandle asset;
};

class RuntimeWorld final : public IRenderWorld {
  public:
    RuntimeWorld() = default;

    static std::unique_ptr<RuntimeWorld>
    fromDocument(const SceneDocument &document,
                 const std::vector<ResolvedModelAsset> &assets);

    SceneDocument toDocument() const;
    void replaceDocument(const SceneDocument &document);
    std::vector<ResolvedModelAsset> resolvedModelAssets() const;

    EntityHandle find(const PersistentEntityId &id) const;
    bool valid(EntityHandle handle) const;
    std::vector<RuntimeEntitySnapshot> entities() const;
    std::optional<RuntimeEntitySnapshot> entity(EntityHandle handle) const;

    EntityHandle createEntity(SceneEntityDocument entity,
                              std::optional<size_t> insertIndex = std::nullopt);
    bool destroyEntity(EntityHandle handle, bool includeDescendants = true);
    bool setParent(EntityHandle handle, std::optional<EntityHandle> parent,
                   ReparentMode mode = ReparentMode::KeepLocal,
                   std::string *error = nullptr);
    bool setName(EntityHandle handle, std::string name);
    bool setEnabled(EntityHandle handle, bool enabled);
    bool setTransform(EntityHandle handle,
                      const SceneTransformDocument &transform);
    bool setModelInstance(EntityHandle handle,
                          std::optional<ModelInstanceDocument> component);
    bool setLight(EntityHandle handle,
                  std::optional<LightComponentDocument> component);
    bool setCamera(EntityHandle handle,
                   std::optional<CameraComponentDocument> component);
    bool setAtmosphere(
        EntityHandle handle,
        std::optional<AtmosphereComponentDocument> component);
    bool setActiveCamera(const PersistentEntityId &id);
    bool bindModel(EntityHandle handle, uint64_t expectedRevision,
                   std::string profileId, ModelAssetHandle asset,
                   std::string error = {});
    uint64_t modelBindingRevision(EntityHandle handle) const;
    std::shared_ptr<const ModelAsset>
    modelAsset(EntityHandle handle) const;

    const SceneDocumentId &id() const { return id_; }
    const std::string &displayName() const { return displayName_; }
    void setIdentity(SceneDocumentId id, std::string displayName);
    const std::optional<PersistentEntityId> &activeCameraId() const {
        return activeCamera_;
    }
    const SceneAmbientDocument &ambient() const { return ambient_; }
    void setAmbient(const SceneAmbientDocument &ambient);
    const std::optional<SceneEnvironmentDocument> &environment() const {
        return environment_;
    }
    void setEnvironment(std::optional<SceneEnvironmentDocument> environment);

    void update(float dt, float time) override;
    void collectRenderCommands(RenderQueue &queue) const override;
    const Bounds &bounds() const override { return bounds_; }
    const std::vector<SceneLight> &lights() const override { return lights_; }
    const std::vector<std::shared_ptr<MaterialInstance>> &
    materials() const override { return materials_; }
    size_t renderableCount() const override;
    bool allowsFallbackSun() const override { return false; }
    std::optional<CameraPose> initialEditorCamera() const override {
        return std::nullopt;
    }
    std::optional<RuntimeCameraView>
    activeCamera(float aspect) const override;
    std::optional<RenderWorldAmbient> worldAmbient() const override;
    std::optional<RenderWorldEnvironment> worldEnvironment() const override;
    std::optional<RenderWorldAtmosphere> worldAtmosphere() const override;

    size_t entityCount() const { return order_.size(); }
    size_t modelInstanceCount() const;
    size_t explicitLightCount() const;

  private:
    struct Slot {
        uint32_t generation = 1;
        bool alive = false;
        PersistentEntityId id;
        std::string name;
        bool enabled = true;
        bool effectiveEnabled = true;
        std::optional<EntityHandle> parent;
        std::vector<EntityHandle> children;
        RuntimeTransformComponent transform;
        std::optional<RuntimeModelInstanceComponent> model;
        std::optional<LightComponentDocument> light;
        std::optional<CameraComponentDocument> camera;
        std::optional<AtmosphereComponentDocument> atmosphere;
    };

    Slot *slot(EntityHandle handle);
    const Slot *slot(EntityHandle handle) const;
    void markDirty(EntityHandle handle);
    void rebuildDerivedState();
    void updateHierarchy(EntityHandle handle, const glm::mat4 &parentWorld,
                         const glm::quat &parentRotation,
                         bool parentEnabled);
    void refreshModelBindingStates();
    bool wouldCreateCycle(EntityHandle handle, EntityHandle parent) const;
    void destroyOne(EntityHandle handle);

    SceneDocumentId id_;
    std::string displayName_;
    std::optional<PersistentEntityId> activeCamera_;
    SceneAmbientDocument ambient_;
    std::optional<SceneEnvironmentDocument> environment_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
    std::vector<EntityHandle> order_;
    std::unordered_map<PersistentEntityId, EntityHandle,
                       PersistentEntityIdHash>
        byId_;
    Bounds bounds_;
    std::vector<SceneLight> lights_;
    std::optional<RenderWorldAtmosphere> atmosphere_;
    std::vector<std::shared_ptr<MaterialInstance>> materials_;
    bool derivedDirty_ = true;
};

const char *modelBindingStateName(ModelBindingState state);

} // namespace vkr
