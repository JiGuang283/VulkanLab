#pragma once

#include "ModelAssetHandle.h"

#include <glm/glm.hpp>

namespace vkr {

struct ModelInstance {
    ModelAssetHandle asset;
    glm::mat4 rootToWorld{1.0f};
    bool visible = true;
};

} // namespace vkr
