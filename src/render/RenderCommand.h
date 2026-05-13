#pragma once

#include <glm/glm.hpp>

namespace vkr {

class MaterialInstance;
class Mesh;

enum class RenderQueueType {
    Opaque,
    Transparent,
};

struct RenderCommand {
    const Mesh             *mesh = nullptr;
    const MaterialInstance *material = nullptr;
    glm::mat4              world{1.0f};
    RenderQueueType queue = RenderQueueType::Opaque;
};

} // namespace vkr

