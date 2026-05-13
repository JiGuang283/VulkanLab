#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace vkr {

class Mesh;
class MaterialInstance;

struct SceneObject {
    std::shared_ptr<Mesh>             mesh;
    std::shared_ptr<MaterialInstance> material;
    glm::mat4                         transform{1.0f};
};

} // namespace vkr
