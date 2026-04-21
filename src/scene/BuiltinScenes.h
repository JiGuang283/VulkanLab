#pragma once

#include "SceneFactory.h"

namespace vkr {

/// Factory that loads `models/viking_room.obj` with the given shared texture
/// and shaders.  Applies a default rotation animation and an overhead camera
/// pose.
SceneFactory vikingRoomSceneFactory(std::string texturePath,
                                    std::string vertShaderPath,
                                    std::string fragShaderPath);

/// Factory for `models/SheenChair.glb` (glTF).  Static (no animation), with
/// a camera pose framing the chair.
SceneFactory sheenChairSceneFactory(std::string texturePath,
                                    std::string vertShaderPath,
                                    std::string fragShaderPath);

} // namespace vkr
