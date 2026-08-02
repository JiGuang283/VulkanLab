#include "ModelLight.h"

#include <cmath>
#include <utility>

namespace vkr {
namespace {

glm::vec3 transformDirection(const glm::mat4 &transform,
                             const glm::vec3 &direction) {
    const glm::vec3 transformed = glm::mat3(transform) * direction;
    const float lengthSquared = glm::dot(transformed, transformed);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
        return direction;
    return transformed / std::sqrt(lengthSquared);
}

} // namespace

SceneLight instantiateModelLight(const ModelLightPrototype &prototype,
                                 const glm::mat4 &rootToWorld,
                                 std::string stableKey,
                                 std::optional<PersistentEntityId>
                                     ownerEntity) {
    SceneLight light{};
    light.debugName = prototype.debugName;
    light.stableKey = std::move(stableKey);
    light.source = SceneLightSource::ImportedModel;
    light.ownerEntity = std::move(ownerEntity);
    light.castsShadow = prototype.castsShadow;
    light.type = prototype.type;
    light.positionWS = glm::vec3(rootToWorld *
                                glm::vec4(prototype.positionAS, 1.0f));
    light.range = prototype.range;
    light.directionWS =
        transformDirection(rootToWorld, prototype.directionAS);
    light.innerConeCos = prototype.innerConeCos;
    light.color = prototype.color;
    light.intensity = prototype.intensity;
    light.outerConeCos = prototype.outerConeCos;
    return light;
}

} // namespace vkr
