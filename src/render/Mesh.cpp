#include "Mesh.h"
#include "TangentGenerator.h"
#include "Vertex.h"
#include "core/Device.h"
#include "core/Log.h"
#include "core/UploadContext.h"
#include "diagnostics/SceneLoadStats.h"

#include <tiny_obj_loader.h>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vkr {

namespace {

Bounds computeBounds(const void *vertexData, VkDeviceSize vertexSize) {
    Bounds bounds{};
    if (!vertexData || vertexSize < sizeof(Vertex))
        return bounds;

    const size_t vertexCount = static_cast<size_t>(vertexSize / sizeof(Vertex));
    const auto  *vertices = static_cast<const Vertex *>(vertexData);
    glm::vec3    minV{std::numeric_limits<float>::max()};
    glm::vec3    maxV{std::numeric_limits<float>::lowest()};

    for (size_t i = 0; i < vertexCount; ++i) {
        minV = glm::min(minV, vertices[i].pos);
        maxV = glm::max(maxV, vertices[i].pos);
    }

    bounds.min = minV;
    bounds.max = maxV;
    bounds.center = (minV + maxV) * 0.5f;
    bounds.radius = glm::length(maxV - bounds.center);
    bounds.valid = true;
    return bounds;
}

} // namespace

Mesh::Mesh(Device &device, UploadContext &upload, const void *vertexData,
           VkDeviceSize vertexSize, const uint32_t *indexData,
           uint32_t indexCount)
    : indexCount_(indexCount) {
    ResourceLoadStats *loadStats = upload.stats();
    localBounds_ = computeBounds(vertexData, vertexSize);
    ScopedLoadTimer uploadTimer(loadStats ? &loadStats->meshUploadMs
                                          : nullptr);

    // ---- 顶点缓冲 ----
    {
        vertexBuffer_ =
            std::make_unique<Buffer>(device, vertexSize,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        upload.uploadBuffer(vertexData, vertexSize, vertexBuffer_->handle());
    }

    // ---- 索引缓冲 ----
    {
        VkDeviceSize indexSize =
            static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

        indexBuffer_ = std::make_unique<Buffer>(
            device, indexSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        upload.uploadBuffer(indexData, indexSize, indexBuffer_->handle());
    }

    if (loadStats) {
        ++loadStats->gpuMeshCount;
        loadStats->vertexCount += vertexSize / sizeof(Vertex);
        loadStats->indexCount += indexCount;
        loadStats->vertexUploadBytes += static_cast<uint64_t>(vertexSize);
        loadStats->indexUploadBytes +=
            static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
    }
}

std::unique_ptr<Mesh> Mesh::fromOBJ(Device &device, UploadContext &upload,
                                    const std::string &path) {
    tinyobj::attrib_t                attrib;
    std::vector<tinyobj::shape_t>    shapes;
    std::vector<tinyobj::material_t> materials;
    std::string                      err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str())) {
        throw std::runtime_error(err);
    }

    std::vector<Vertex>                  vertices;
    std::vector<uint32_t>                indices;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    for (const auto &shape : shapes) {
        for (const auto &index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.pos = {attrib.vertices[3 * index.vertex_index + 0],
                          attrib.vertices[3 * index.vertex_index + 1],
                          attrib.vertices[3 * index.vertex_index + 2]};
            if (!attrib.texcoords.empty() && index.texcoord_index >= 0) {
                vertex.texCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
            } else {
                vertex.texCoord = {0.0f, 0.0f};
            }
            vertex.texCoord1 = vertex.texCoord;
            if (!attrib.normals.empty() && index.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2],
                };
            } else {
                vertex.normal = {0.0f, 1.0f, 0.0f};
            }

            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }
            indices.push_back(uniqueVertices[vertex]);
        }
    }

    generateTangents(vertices, indices);

    VKR_LOG_DEBUG("Mesh", "Loaded OBJ '{}': vertices={}, indices={}", path,
                  vertices.size(), indices.size());

    return std::make_unique<Mesh>(
        device, upload, vertices.data(),
        static_cast<VkDeviceSize>(sizeof(Vertex) * vertices.size()),
        indices.data(), static_cast<uint32_t>(indices.size()));
}

void Mesh::bind(VkCommandBuffer cmd) const {
    VkBuffer     buffers[] = {vertexBuffer_->handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

} // namespace vkr
