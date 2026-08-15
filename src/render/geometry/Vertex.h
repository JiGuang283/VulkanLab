#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>    // offsetof
#include <functional> // std::hash

namespace vkr {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 texCoord1{0.0f, 0.0f};
    glm::vec4 color{1.0f};

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 6>
    getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 6> attrs{};

        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, pos);

        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, normal);

        attrs[2].binding = 0;
        attrs[2].location = 2;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = offsetof(Vertex, texCoord);

        attrs[3].binding = 0;
        attrs[3].location = 3;
        attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[3].offset = offsetof(Vertex, tangent);

        attrs[4].binding = 0;
        attrs[4].location = 4;
        attrs[4].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[4].offset = offsetof(Vertex, texCoord1);

        attrs[5].binding = 0;
        attrs[5].location = 5;
        attrs[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[5].offset = offsetof(Vertex, color);

        return attrs;
    }

    bool operator==(const Vertex &other) const {
        return pos == other.pos && normal == other.normal &&
               texCoord == other.texCoord && tangent == other.tangent &&
               texCoord1 == other.texCoord1 && color == other.color;
    }
};

} // namespace vkr

// hash 特化必须在全局 std 命名空间中
namespace std {
template <> struct hash<vkr::Vertex> {
    size_t operator()(vkr::Vertex const &vertex) const {
        size_t seed = hash<glm::vec3>()(vertex.pos);
        seed ^= hash<glm::vec3>()(vertex.normal) + 0x9e3779b9u +
                (seed << 6) + (seed >> 2);
        seed ^= hash<glm::vec2>()(vertex.texCoord) + 0x9e3779b9u +
                (seed << 6) + (seed >> 2);
        seed ^= hash<glm::vec4>()(vertex.tangent) + 0x9e3779b9u +
                (seed << 6) + (seed >> 2);
        seed ^= hash<glm::vec2>()(vertex.texCoord1) + 0x9e3779b9u +
                (seed << 6) + (seed >> 2);
        seed ^= hash<glm::vec4>()(vertex.color) + 0x9e3779b9u +
                (seed << 6) + (seed >> 2);
        return seed;
    }
};
} // namespace std
