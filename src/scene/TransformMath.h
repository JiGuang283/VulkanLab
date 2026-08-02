#pragma once

#include "scene_data/SceneDocument.h"

#include <glm/glm.hpp>

namespace vkr {

glm::mat4 composeSceneTransform(const SceneTransformDocument &transform);

bool decomposeSceneTransform(const glm::mat4 &matrix,
                             SceneTransformDocument &transform,
                             float tolerance = 1.0e-4f);

} // namespace vkr
