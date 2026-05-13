#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vkr {

enum class MaterialTextureSlot : uint32_t {
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4,
    Count,
};

inline constexpr size_t kMaterialTextureSlotCount =
    static_cast<size_t>(MaterialTextureSlot::Count);

template <typename T>
using MaterialTextureSlotArray = std::array<T, kMaterialTextureSlotCount>;

inline constexpr uint32_t bindingFor(MaterialTextureSlot slot) {
    return static_cast<uint32_t>(slot);
}

inline constexpr size_t indexOf(MaterialTextureSlot slot) {
    return static_cast<size_t>(slot);
}

} // namespace vkr
