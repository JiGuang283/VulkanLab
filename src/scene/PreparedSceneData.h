#pragma once

#include "render/MaterialInstance.h"
#include "render/MaterialTextureSlot.h"
#include "render/Vertex.h"
#include "SceneTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

enum class PreparedTextureDataKind { RawBaseLevel, PrebuiltMipChain };

struct PreparedMipLevel {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PreparedImage {
    std::vector<uint8_t> pixels;
    uint32_t             width = 0;
    uint32_t             height = 0;
    VkFormat             format = VK_FORMAT_UNDEFINED;
    PreparedTextureDataKind kind = PreparedTextureDataKind::RawBaseLevel;
    std::vector<PreparedMipLevel> mipLevels;
};

struct PreparedTexture {
    std::shared_ptr<const PreparedImage> image;
    std::string          debugName;
    VkFormat             format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFilter             minFilter = VK_FILTER_LINEAR;
    VkFilter             magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode  mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
};

struct PreparedMaterial {
    MaterialParams params;
    std::array<int32_t, kMaterialTextureSlotCount> textureIndices{};

    PreparedMaterial() { textureIndices.fill(-1); }
};

struct PreparedMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    Bounds                bounds;
    std::string           debugName;
};

struct PreparedObject {
    uint32_t  meshIndex = 0;
    int32_t   materialIndex = -1;
    glm::mat4 transform{1.0f};
};

struct PreparedSceneData {
    std::string sourcePath;
    std::vector<PreparedTexture>  textures;
    std::vector<PreparedMaterial> materials;
    std::vector<PreparedMesh>     meshes;
    std::vector<PreparedObject>   objects;
    std::optional<CameraPose>     initialCamera;
};

} // namespace vkr
