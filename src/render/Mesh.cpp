#include "Mesh.h"
#include "TangentGenerator.h"
#include "Vertex.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"

#include <tiny_obj_loader.h>

#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vkr {

Mesh::Mesh(Device &device, FrameSync &frameSync, const void *vertexData,
           VkDeviceSize vertexSize, const uint32_t *indexData,
           uint32_t indexCount)
    : indexCount_(indexCount) {

    // ---- 顶点缓冲 ----
    {
        Buffer staging(device, vertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void  *mapped = staging.map();
        memcpy(mapped, vertexData, vertexSize);
        staging.unmap();

        vertexBuffer_ =
            std::make_unique<Buffer>(device, vertexSize,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        frameSync.copyBuffer(staging.handle(), vertexBuffer_->handle(),
                             vertexSize);
    }

    // ---- 索引缓冲 ----
    {
        VkDeviceSize indexSize =
            static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

        Buffer staging(device, indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void  *mapped = staging.map();
        memcpy(mapped, indexData, indexSize);
        staging.unmap();

        indexBuffer_ = std::make_unique<Buffer>(
            device, indexSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        frameSync.copyBuffer(staging.handle(), indexBuffer_->handle(),
                             indexSize);
    }
}

std::unique_ptr<Mesh> Mesh::fromOBJ(Device &device, FrameSync &frameSync,
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
        device, frameSync, vertices.data(),
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
