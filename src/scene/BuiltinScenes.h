#pragma once

#include "Scene.h"
#include "SceneFactory.h"

#include <optional>

namespace vkr {

/// Factory that loads `models/viking_room.obj` with the given shared texture
/// and shaders.  Applies a default rotation animation and an overhead camera
/// pose.
SceneFactory vikingRoomSceneFactory(std::string texturePath,
                                    std::string vertShaderPath,
                                    std::string fragShaderPath);

/// Factory for `models/SheenChair.glb` (glTF).  Static (no animation); all
/// textures/materials come from the glb itself.
SceneFactory sheenChairSceneFactory(std::string vertShaderPath,
                                    std::string fragShaderPath);

/// Generic glTF scene factory.  Loads any .glb/.gltf at `modelPath`.
SceneFactory gltfSceneFactory(std::string modelPath, std::string vertShaderPath,
                              std::string fragShaderPath,
                              std::optional<CameraPose> cameraOverride =
                                  std::nullopt);

} // namespace vkr
