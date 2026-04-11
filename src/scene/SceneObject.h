#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace vkr {

class Mesh;
class Material;

struct SceneObject {
    std::shared_ptr<Mesh>     mesh;
    std::shared_ptr<Material> material;
    glm::mat4                 transform{1.0f};
};

} // namespace vkr
