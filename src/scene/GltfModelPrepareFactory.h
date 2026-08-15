#pragma once

#include "ModelPrepareFactory.h"
#include "scene_data/SceneTypes.h"

#include <optional>

namespace vkr {

ModelPrepareFactory gltfModelPrepareFactory(
    std::string modelPath,
    std::optional<CameraPose> cameraOverride = std::nullopt);

} // namespace vkr
