#pragma once

#include "ModelLight.h"
#include "scene_data/SceneTypes.h"
#include "scene_data/SceneIds.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

class MaterialInstance;
class MaterialTemplate;
class Mesh;
class Texture;

struct ModelPrimitive {
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<MaterialInstance> material;
    glm::mat4 localToAsset{1.0f};
};

struct ModelAsset {
    ModelAssetId id;
    std::string profileId;
    uint64_t generation = 0;
    std::shared_ptr<MaterialTemplate> materialTemplate;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<std::shared_ptr<Mesh>> meshes;
    std::vector<std::shared_ptr<MaterialInstance>> materials;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelLightPrototype> lights;
    Bounds localBounds;
    std::optional<CameraPose> previewCamera;
};

} // namespace vkr
