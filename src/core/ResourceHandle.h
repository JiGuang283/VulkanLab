#pragma once

#include <cstdint>
#include <limits>

namespace vkr {

template <typename Tag> struct Handle {
    uint32_t index = invalidIndex();
    uint32_t generation = 0;

    static constexpr uint32_t invalidIndex() {
        return std::numeric_limits<uint32_t>::max();
    }

    bool     valid() const { return index != invalidIndex(); }
    explicit operator bool() const { return valid(); }

    friend bool operator==(Handle lhs, Handle rhs) {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }

    friend bool operator!=(Handle lhs, Handle rhs) { return !(lhs == rhs); }
};

struct TextureTag {};
struct MeshTag {};
struct MaterialTag {};
struct PipelineTag {};

} // namespace vkr