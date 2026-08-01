#pragma once

#include "scene_data/SceneIds.h"

#include <functional>
#include <string>
#include <vector>

namespace vkr {

struct OutlinerEntitySnapshot {
    PersistentEntityId id;
    std::string name;
    int depth = 0;
    bool enabled = true;
};

struct OutlinerPanelActions {
    std::function<void(PersistentEntityId)> selectEntity;
};

// Stage 1 defines the data boundary only. Drawing is enabled with RuntimeWorld
// and native SceneDocument loading in Stage 3.
class OutlinerPanel {
  public:
    using Snapshot = std::vector<OutlinerEntitySnapshot>;
};

} // namespace vkr
