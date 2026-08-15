#include "PrimitiveMeshGenerator.h"

#include "render/geometry/TangentGenerator.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kRadialSegments = 32;
constexpr uint32_t kSphereStacks = 16;
constexpr uint32_t kCapsuleHemisphereSegments = 8;

Vertex vertex(const glm::vec3 &position, const glm::vec3 &normal,
              const glm::vec2 &uv) {
    Vertex result{};
    result.pos = position;
    result.normal = normal;
    result.texCoord = uv;
    result.texCoord1 = uv;
    result.color = glm::vec4(1.0f);
    return result;
}

void appendQuad(PreparedMesh &mesh, const glm::vec3 &p0,
                const glm::vec3 &p1, const glm::vec3 &p2,
                const glm::vec3 &p3, const glm::vec3 &normal) {
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(vertex(p0, normal, {0.0f, 0.0f}));
    mesh.vertices.push_back(vertex(p1, normal, {1.0f, 0.0f}));
    mesh.vertices.push_back(vertex(p2, normal, {1.0f, 1.0f}));
    mesh.vertices.push_back(vertex(p3, normal, {0.0f, 1.0f}));
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2,
                         base + 3});
}

void appendCap(PreparedMesh &mesh, float z, float radius, bool top) {
    const glm::vec3 normal = top ? glm::vec3(0.0f, 0.0f, 1.0f)
                                 : glm::vec3(0.0f, 0.0f, -1.0f);
    const uint32_t center = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(vertex({0.0f, 0.0f, z}, normal, {0.5f, 0.5f}));
    const uint32_t ring = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t slice = 0; slice <= kRadialSegments; ++slice) {
        const float angle = 2.0f * kPi * static_cast<float>(slice) /
                            static_cast<float>(kRadialSegments);
        const float x = radius * std::cos(angle);
        const float y = radius * std::sin(angle);
        mesh.vertices.push_back(vertex(
            {x, y, z}, normal,
            {x / (2.0f * radius) + 0.5f,
             y / (2.0f * radius) + 0.5f}));
    }
    for (uint32_t slice = 0; slice < kRadialSegments; ++slice) {
        if (top) {
            mesh.indices.insert(mesh.indices.end(),
                                {center, ring + slice, ring + slice + 1});
        } else {
            mesh.indices.insert(mesh.indices.end(),
                                {center, ring + slice + 1, ring + slice});
        }
    }
}

PreparedMesh planeMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Plane";
    appendQuad(mesh, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f},
               {0.5f, 0.5f, 0.0f}, {-0.5f, 0.5f, 0.0f},
               {0.0f, 0.0f, 1.0f});
    return mesh;
}

PreparedMesh cubeMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Cube";
    appendQuad(mesh, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
               {0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
               {1.0f, 0.0f, 0.0f});
    appendQuad(mesh, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
               {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
               {-1.0f, 0.0f, 0.0f});
    appendQuad(mesh, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
               {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f},
               {0.0f, 1.0f, 0.0f});
    appendQuad(mesh, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
               {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f},
               {0.0f, -1.0f, 0.0f});
    appendQuad(mesh, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
               {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
               {0.0f, 0.0f, 1.0f});
    appendQuad(mesh, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
               {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
               {0.0f, 0.0f, -1.0f});
    return mesh;
}

PreparedMesh sphereMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Sphere";
    constexpr float radius = 0.5f;
    for (uint32_t stack = 0; stack <= kSphereStacks; ++stack) {
        const float v = static_cast<float>(stack) /
                        static_cast<float>(kSphereStacks);
        const float theta = kPi * v;
        const float radial = std::sin(theta);
        const float z = std::cos(theta);
        for (uint32_t slice = 0; slice <= kRadialSegments; ++slice) {
            const float u = static_cast<float>(slice) /
                            static_cast<float>(kRadialSegments);
            const float phi = 2.0f * kPi * u;
            const glm::vec3 normal{radial * std::cos(phi),
                                   radial * std::sin(phi), z};
            mesh.vertices.push_back(vertex(radius * normal, normal, {u, v}));
        }
    }
    const uint32_t stride = kRadialSegments + 1;
    for (uint32_t stack = 0; stack < kSphereStacks; ++stack) {
        for (uint32_t slice = 0; slice < kRadialSegments; ++slice) {
            const uint32_t a = stack * stride + slice;
            const uint32_t b = (stack + 1) * stride + slice;
            if (stack != kSphereStacks - 1)
                mesh.indices.insert(mesh.indices.end(),
                                    {a, b, b + 1});
            if (stack != 0)
                mesh.indices.insert(mesh.indices.end(),
                                    {a, b + 1, a + 1});
        }
    }
    return mesh;
}

PreparedMesh cylinderMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Cylinder";
    constexpr float radius = 0.5f;
    constexpr float halfHeight = 0.5f;
    for (uint32_t slice = 0; slice <= kRadialSegments; ++slice) {
        const float u = static_cast<float>(slice) /
                        static_cast<float>(kRadialSegments);
        const float angle = 2.0f * kPi * u;
        const glm::vec3 normal{std::cos(angle), std::sin(angle), 0.0f};
        mesh.vertices.push_back(
            vertex(radius * normal - glm::vec3(0.0f, 0.0f, halfHeight),
                   normal, {u, 0.0f}));
        mesh.vertices.push_back(
            vertex(radius * normal + glm::vec3(0.0f, 0.0f, halfHeight),
                   normal, {u, 1.0f}));
    }
    for (uint32_t slice = 0; slice < kRadialSegments; ++slice) {
        const uint32_t bottom = slice * 2;
        const uint32_t top = bottom + 1;
        const uint32_t nextBottom = bottom + 2;
        const uint32_t nextTop = bottom + 3;
        mesh.indices.insert(mesh.indices.end(),
                            {bottom, nextBottom, nextTop, bottom, nextTop,
                             top});
    }
    appendCap(mesh, halfHeight, radius, true);
    appendCap(mesh, -halfHeight, radius, false);
    return mesh;
}

PreparedMesh coneMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Cone";
    constexpr float radius = 0.5f;
    constexpr float halfHeight = 0.5f;
    for (uint32_t slice = 0; slice < kRadialSegments; ++slice) {
        const float u0 = static_cast<float>(slice) /
                         static_cast<float>(kRadialSegments);
        const float u1 = static_cast<float>(slice + 1) /
                         static_cast<float>(kRadialSegments);
        const float a0 = 2.0f * kPi * u0;
        const float a1 = 2.0f * kPi * u1;
        const float mid = 0.5f * (a0 + a1);
        const glm::vec3 n0 = glm::normalize(
            glm::vec3(std::cos(a0), std::sin(a0), radius));
        const glm::vec3 n1 = glm::normalize(
            glm::vec3(std::cos(a1), std::sin(a1), radius));
        const glm::vec3 apexNormal = glm::normalize(
            glm::vec3(std::cos(mid), std::sin(mid), radius));
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vertex(
            {radius * std::cos(a0), radius * std::sin(a0), -halfHeight},
            n0, {u0, 0.0f}));
        mesh.vertices.push_back(vertex(
            {radius * std::cos(a1), radius * std::sin(a1), -halfHeight},
            n1, {u1, 0.0f}));
        mesh.vertices.push_back(vertex({0.0f, 0.0f, halfHeight},
                                       apexNormal,
                                       {(u0 + u1) * 0.5f, 1.0f}));
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2});
    }
    appendCap(mesh, -halfHeight, radius, false);
    return mesh;
}

PreparedMesh capsuleMesh() {
    PreparedMesh mesh;
    mesh.debugName = "Capsule";
    constexpr float radius = 0.5f;
    constexpr float halfCylinderHeight = 0.5f;

    struct Ring {
        float radial;
        float z;
        float normalRadial;
        float normalZ;
    };
    std::vector<Ring> rings;
    rings.reserve(2 * (kCapsuleHemisphereSegments + 1));
    for (uint32_t index = 0; index <= kCapsuleHemisphereSegments; ++index) {
        const float theta = 0.5f * kPi * static_cast<float>(index) /
                            static_cast<float>(kCapsuleHemisphereSegments);
        rings.push_back({radius * std::sin(theta),
                         halfCylinderHeight + radius * std::cos(theta),
                         std::sin(theta), std::cos(theta)});
    }
    for (uint32_t index = 0; index <= kCapsuleHemisphereSegments; ++index) {
        const float theta = 0.5f * kPi +
                            0.5f * kPi * static_cast<float>(index) /
                                static_cast<float>(
                                    kCapsuleHemisphereSegments);
        rings.push_back({radius * std::sin(theta),
                         -halfCylinderHeight + radius * std::cos(theta),
                         std::sin(theta), std::cos(theta)});
    }

    for (size_t ringIndex = 0; ringIndex < rings.size(); ++ringIndex) {
        const Ring &ring = rings[ringIndex];
        const float v = static_cast<float>(ringIndex) /
                        static_cast<float>(rings.size() - 1);
        for (uint32_t slice = 0; slice <= kRadialSegments; ++slice) {
            const float u = static_cast<float>(slice) /
                            static_cast<float>(kRadialSegments);
            const float angle = 2.0f * kPi * u;
            const glm::vec3 normal{
                ring.normalRadial * std::cos(angle),
                ring.normalRadial * std::sin(angle), ring.normalZ};
            mesh.vertices.push_back(vertex(
                {ring.radial * std::cos(angle),
                 ring.radial * std::sin(angle), ring.z},
                normal, {u, v}));
        }
    }

    const uint32_t stride = kRadialSegments + 1;
    for (uint32_t ring = 0; ring + 1 < rings.size(); ++ring) {
        for (uint32_t slice = 0; slice < kRadialSegments; ++slice) {
            const uint32_t a = ring * stride + slice;
            const uint32_t b = (ring + 1) * stride + slice;
            if (ring + 1 != rings.size() - 1)
                mesh.indices.insert(mesh.indices.end(),
                                    {a, b, b + 1});
            if (ring != 0)
                mesh.indices.insert(mesh.indices.end(),
                                    {a, b + 1, a + 1});
        }
    }
    return mesh;
}

void finalize(PreparedMesh &mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty())
        throw std::runtime_error("Generated primitive mesh is empty");
    generateTangents(mesh.vertices, mesh.indices);

    glm::vec3 minValue(std::numeric_limits<float>::max());
    glm::vec3 maxValue(std::numeric_limits<float>::lowest());
    for (const Vertex &item : mesh.vertices) {
        minValue = glm::min(minValue, item.pos);
        maxValue = glm::max(maxValue, item.pos);
    }
    mesh.bounds.min = minValue;
    mesh.bounds.max = maxValue;
    mesh.bounds.center = (minValue + maxValue) * 0.5f;
    mesh.bounds.radius = glm::length(maxValue - mesh.bounds.center);
    mesh.bounds.valid = true;
}

} // namespace

PreparedMesh generatePrimitiveMesh(PrimitiveType type) {
    PreparedMesh mesh;
    switch (type) {
    case PrimitiveType::Plane:
        mesh = planeMesh();
        break;
    case PrimitiveType::Cube:
        mesh = cubeMesh();
        break;
    case PrimitiveType::Sphere:
        mesh = sphereMesh();
        break;
    case PrimitiveType::Cylinder:
        mesh = cylinderMesh();
        break;
    case PrimitiveType::Cone:
        mesh = coneMesh();
        break;
    case PrimitiveType::Capsule:
        mesh = capsuleMesh();
        break;
    }
    finalize(mesh);
    return mesh;
}

} // namespace vkr
