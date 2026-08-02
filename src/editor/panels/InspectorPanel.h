#pragma once

#include "scene/RuntimeWorld.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct InspectorModelOption {
    std::string id;
    std::string displayName;
    bool instanceable = true;
};

struct InspectorEnvironmentOption {
    std::string id;
    std::string displayName;
};

struct InspectorPanelSnapshot {
    std::optional<RuntimeEntitySnapshot> entity;
    SceneDocumentId sceneId;
    std::string sceneDisplayName;
    SceneAmbientDocument ambient;
    std::optional<SceneEnvironmentDocument> environment;
    std::optional<PersistentEntityId> activeCamera;
    std::vector<RuntimeEntitySnapshot> entities;
    std::vector<RuntimeEntitySnapshot> cameraEntities;
    std::vector<InspectorModelOption> models;
    std::vector<InspectorEnvironmentOption> environments;
    bool selectedLightLimitExceeded = false;
    bool editable = false;
};

struct InspectorPanelActions {
    std::function<void(PersistentEntityId, std::string)> setName;
    std::function<void(PersistentEntityId, bool)> setEnabled;
    std::function<void(PersistentEntityId,
                       std::optional<PersistentEntityId>)>
        setParent;
    std::function<void(PersistentEntityId, SceneTransformDocument)>
        setTransform;
    std::function<void(PersistentEntityId,
                       std::optional<ModelInstanceDocument>)>
        setModel;
    std::function<void(PersistentEntityId,
                       std::optional<LightComponentDocument>)>
        setLight;
    std::function<void(PersistentEntityId,
                       std::optional<CameraComponentDocument>)>
        setCamera;
    std::function<void(PersistentEntityId)> setActiveCamera;
    std::function<void(SceneAmbientDocument)> setAmbient;
    std::function<void(std::optional<SceneEnvironmentDocument>)>
        setEnvironment;
    std::function<void(std::string)> beginContinuous;
    std::function<void()> commitContinuous;
};

class InspectorPanel {
  public:
    void draw(const InspectorPanelSnapshot &snapshot,
              const InspectorPanelActions &actions);

  private:
    bool transformEditing_ = false;
    bool lightEditing_ = false;
    bool cameraEditing_ = false;
    bool sceneEditing_ = false;
};

} // namespace vkr
