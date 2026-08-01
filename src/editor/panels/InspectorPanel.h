#pragma once

#include "scene_data/SceneIds.h"

#include <optional>
#include <string>

namespace vkr {

struct InspectorSelectionSnapshot {
    PersistentEntityId id;
    std::string name;
};

// Stage 1 reserves the selection contract without exposing editable runtime
// components before RuntimeWorld exists.
class InspectorPanel {
  public:
    using Snapshot = std::optional<InspectorSelectionSnapshot>;
};

} // namespace vkr
