#pragma once

#include "scene_data/SceneIds.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vkr {

struct AtmosphereRuntimeStatus {
    bool supported = false;
    bool componentPresent = false;
    bool active = false;
    bool staticLutReady = false;
    bool staticLutDirty = false;
    std::string unavailableReason;
    PersistentEntityId componentEntity;
    std::optional<PersistentEntityId> sunEntity;
    int32_t sunBufferIndex = -1;
    float cameraAltitudeKm = 0.0f;
    uint64_t lutGeneration = 0;
    double lastUpdateMs = 0.0;
};

} // namespace vkr
