#pragma once

#include "scene/ModelLightPrototype.h"
#include "render/frame/SceneLight.h"

#include <glm/glm.hpp>
namespace vkr {

SceneLight instantiateModelLight(const ModelLightPrototype &prototype,
                                 const glm::mat4 &rootToWorld,
                                 std::string stableKey,
                                 std::optional<PersistentEntityId>
                                     ownerEntity = std::nullopt);

} // namespace vkr
