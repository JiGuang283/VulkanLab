#pragma once

#include "SceneFactory.h"
#include "SceneTypes.h"

#include <optional>

namespace vkr {

/// Factory that loads the given Viking Room OBJ and shared texture. Applies a
/// default rotation animation and an overhead camera pose.
SceneFactory vikingRoomSceneFactory(std::string modelPath,
                                    std::string texturePath);

/// Generic glTF scene factory.  Loads any .glb/.gltf at `modelPath`.
ScenePrepareFactory gltfSceneFactory(
    std::string modelPath,
    std::optional<CameraPose> cameraOverride = std::nullopt);

} // namespace vkr
