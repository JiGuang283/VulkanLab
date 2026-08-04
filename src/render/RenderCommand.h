#pragma once

#include "scene/SceneTypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <limits>

namespace vkr {

class MaterialInstance;
class Mesh;

enum class RenderQueueType {
    Opaque,
    Transparent,
};

using RenderItemId = uint64_t;

struct RenderCommand {
    const Mesh             *mesh = nullptr;
    const MaterialInstance *material = nullptr;
    glm::mat4              world{1.0f};
    RenderQueueType queue = RenderQueueType::Opaque;
    Bounds localBounds{};
    Bounds worldBounds{};
    RenderItemId renderItemId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t sourceOrder = std::numeric_limits<uint32_t>::max();
    uint32_t occlusionSlot = std::numeric_limits<uint32_t>::max();
};

} // namespace vkr

