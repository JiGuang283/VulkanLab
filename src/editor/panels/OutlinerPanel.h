#pragma once

#include "scene/RuntimeWorld.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class OutlinerCreateKind {
    Empty,
    Model,
    DirectionalLight,
    PointLight,
    SpotLight,
    Camera,
};

struct OutlinerEntitySnapshot {
    PersistentEntityId id;
    std::optional<PersistentEntityId> parent;
    std::string name;
    bool enabled = true;
    bool selected = false;
    ModelBindingState modelState = ModelBindingState::Unresolved;
    bool hasModel = false;
    bool lightLimitExceeded = false;
};

struct OutlinerPanelSnapshot {
    std::vector<OutlinerEntitySnapshot> entities;
    bool editable = false;
};

struct OutlinerPanelActions {
    std::function<void(std::optional<PersistentEntityId>)> select;
    std::function<void(OutlinerCreateKind,
                       std::optional<PersistentEntityId>)>
        create;
    std::function<void(PersistentEntityId, std::string)> rename;
    std::function<void(PersistentEntityId, bool)> setEnabled;
    std::function<void(PersistentEntityId)> duplicate;
    std::function<void(PersistentEntityId)> remove;
};

class OutlinerPanel {
  public:
    void draw(const OutlinerPanelSnapshot &snapshot,
              const OutlinerPanelActions &actions);
    void beginRename(const PersistentEntityId &id,
                     const std::string &currentName);

  private:
    void drawEntity(const OutlinerEntitySnapshot &entity,
                    const OutlinerPanelSnapshot &snapshot,
                    const OutlinerPanelActions &actions);
    void drawCreateMenu(const OutlinerPanelActions &actions,
                        std::optional<PersistentEntityId> parent);

    std::array<char, 128> search_{};
    std::array<char, 192> renameBuffer_{};
    std::optional<PersistentEntityId> renameTarget_;
};

} // namespace vkr
