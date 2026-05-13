#pragma once

#include "scene/Scene.h"       // CameraPose
#include "scene/SceneObject.h" // SceneObject

#include <memory>
#include <optional>
#include <vector>

namespace vkr {

class Texture;
class MaterialInstance;
class Mesh;

/// glTF 一次加载的全部产出：贴图、材质、网格、已展开的实例（带 world matrix）
/// 以及（可选）作者建议的相机位姿。
/// 由 GltfLoader 产生（见 Step 5），由场景工厂消费（见 Step 7）。
struct GltfAsset {
    std::vector<std::shared_ptr<Texture>>  textures;
    std::vector<std::shared_ptr<MaterialInstance>> materials;
    std::vector<std::shared_ptr<Mesh>>     meshes; // 一个 primitive → 一个 Mesh
    std::vector<SceneObject>               objects; // 节点展开后的实例
    std::optional<CameraPose>              suggestedCamera;
};

} // namespace vkr
