#pragma once

#include "assets/PreparedTextureData.h"
#include "scene/ModelLightPrototype.h"
#include "scene_data/SceneTypes.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialTextureSlot.h"
#include "render/geometry/Vertex.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct PreparedTexture {
    std::shared_ptr<const PreparedImage> image;
    std::string debugName;
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkFilter magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
};

struct PreparedMaterial {
    MaterialParams params;
    std::string shaderFamilyId;
    std::array<int32_t, kMaterialTextureSlotCount> textureIndices{};

    PreparedMaterial() { textureIndices.fill(-1); }
};

struct PreparedMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Bounds bounds;
    std::string debugName;
};

struct PreparedModelPrimitive {
    uint32_t meshIndex = 0;
    int32_t materialIndex = -1;
    glm::mat4 localToAsset{1.0f};
};

struct PreparedModelData {
    std::string sourcePath;
    std::vector<PreparedTexture> textures;
    std::vector<PreparedMaterial> materials;
    std::vector<PreparedMesh> meshes;
    std::vector<PreparedModelPrimitive> primitives;
    std::vector<ModelLightPrototype> lights;
    std::optional<CameraPose> previewCamera;
};

} // namespace vkr
