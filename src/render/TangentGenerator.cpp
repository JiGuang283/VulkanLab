#include "TangentGenerator.h"

#include <glm/geometric.hpp>
#include <glm/gtx/norm.hpp>

#include <cmath>

namespace vkr {

namespace {

constexpr float kEpsilon = 1.0e-6f;

bool finiteVec3(const glm::vec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

glm::vec3 normalizeOr(const glm::vec3 &v, const glm::vec3 &fallback) {
    if (!finiteVec3(v) || glm::length2(v) <= kEpsilon)
        return fallback;
    return glm::normalize(v);
}

glm::vec3 fallbackTangentFor(const glm::vec3 &normal) {
    const glm::vec3 n = normalizeOr(normal, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 axis =
        std::abs(n.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f)
                               : glm::vec3(0.0f, 1.0f, 0.0f);
    return normalizeOr(glm::cross(axis, n), glm::vec3(1.0f, 0.0f, 0.0f));
}

} // namespace

void generateTangents(std::vector<Vertex> &vertices,
                      const std::vector<uint32_t> &indices) {
    std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() ||
            i2 >= vertices.size()) {
            continue;
        }

        const Vertex &v0 = vertices[i0];
        const Vertex &v1 = vertices[i1];
        const Vertex &v2 = vertices[i2];

        const glm::vec3 e1 = v1.pos - v0.pos;
        const glm::vec3 e2 = v2.pos - v0.pos;
        const glm::vec2 duv1 = v1.texCoord - v0.texCoord;
        const glm::vec2 duv2 = v2.texCoord - v0.texCoord;

        const float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(det) <= kEpsilon)
            continue;

        const float invDet = 1.0f / det;
        const glm::vec3 tangent = (e1 * duv2.y - e2 * duv1.y) * invDet;
        const glm::vec3 bitangent = (e2 * duv1.x - e1 * duv2.x) * invDet;
        if (!finiteVec3(tangent) || !finiteVec3(bitangent))
            continue;

        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;
        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        Vertex &vertex = vertices[i];
        const glm::vec3 n =
            normalizeOr(vertex.normal, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 fallback = fallbackTangentFor(n);

        glm::vec3 tangent = tangents[i] - n * glm::dot(n, tangents[i]);
        tangent = normalizeOr(tangent, fallback);

        const glm::vec3 bitangent = bitangents[i];
        const float handedness =
            glm::length2(bitangent) > kEpsilon &&
                    glm::dot(glm::cross(n, tangent), bitangent) < 0.0f
                ? -1.0f
                : 1.0f;

        vertex.normal = n;
        vertex.tangent = glm::vec4(tangent, handedness);
    }
}

} // namespace vkr
