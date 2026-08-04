#pragma once

#include "scene/SceneTypes.h"
#include "scene_data/SceneIds.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <limits>

namespace vkr {

class MaterialInstance;
class Mesh;

enum class RenderQueueType {
    Opaque,
    Transparent,
};

enum class RenderItemOwnerKind : uint8_t {
    NativeEntity,
    PreviewInstance,
    LegacyObject,
};

struct RenderItemKey {
    RenderItemOwnerKind ownerKind = RenderItemOwnerKind::LegacyObject;
    PersistentEntityId entityId{};
    uint64_t assetGeneration = 0;
    uint32_t primitiveIndex = 0;
    uint32_t fallbackOrdinal = 0;

    friend bool operator==(const RenderItemKey &left,
                           const RenderItemKey &right) {
        return left.ownerKind == right.ownerKind &&
               left.entityId == right.entityId &&
               left.assetGeneration == right.assetGeneration &&
               left.primitiveIndex == right.primitiveIndex &&
               left.fallbackOrdinal == right.fallbackOrdinal;
    }
};

struct RenderItemKeyHash {
    size_t operator()(const RenderItemKey &key) const noexcept {
        size_t seed = static_cast<size_t>(key.ownerKind);
        const auto combine = [&seed](size_t value) {
            seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        };
        combine(PersistentEntityIdHash{}(key.entityId));
        combine(std::hash<uint64_t>{}(key.assetGeneration));
        combine(std::hash<uint32_t>{}(key.primitiveIndex));
        combine(std::hash<uint32_t>{}(key.fallbackOrdinal));
        return seed;
    }
};

using RenderItemIndex = uint32_t;

struct RenderItem {
    const Mesh             *mesh = nullptr;
    const MaterialInstance *material = nullptr;
    glm::mat4              world{1.0f};
    glm::mat4              previousWorld{1.0f};
    RenderQueueType queue = RenderQueueType::Opaque;
    Bounds localBounds{};
    Bounds worldBounds{};
    RenderItemKey key{};
    uint32_t primitiveIndex = 0;
    uint32_t sourceOrder = std::numeric_limits<uint32_t>::max();
    bool historyValid = false;
};

// Transitional alias for code outside the canonical visibility path.
using RenderCommand = RenderItem;

} // namespace vkr

