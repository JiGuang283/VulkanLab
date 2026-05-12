#pragma once

#include "SceneObject.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace vkr {

class Mesh;
class Material;
class RenderQueue;
class Texture;

struct CameraPose {
    glm::vec3 position{2.0f, 2.0f, 2.0f};
    float     yaw = -135.0f;
    float     pitch = -30.0f;
};

class Scene {
  public:
    using UpdateFn = std::function<void(Scene &, float dt, float time)>;

    // ---- 资源装填（由场景工厂在构造期调用） ----
    void addTexture(std::shared_ptr<Texture> t) {
        textures_.push_back(std::move(t));
    }
    void addMaterial(std::shared_ptr<Material> m) {
        materials_.push_back(std::move(m));
    }
    void addMesh(std::shared_ptr<Mesh> m) { meshes_.push_back(std::move(m)); }
    void addObject(SceneObject obj);

    // ---- 渲染提交 ----
    void collectRenderCommands(RenderQueue &queue) const;

    // ---- 每帧 tick（可选） ----
    void setUpdateFn(UpdateFn fn) { updateFn_ = std::move(fn); }
    void update(float dt, float time) {
        if (updateFn_)
            updateFn_(*this, dt, time);
    }

    // ---- 访问器 ----
    std::vector<SceneObject>       &objects() { return objects_; }
    const std::vector<SceneObject> &objects() const { return objects_; }

    std::optional<CameraPose> initialCamera;

  private:
    std::vector<std::shared_ptr<Texture>>  textures_;
    std::vector<std::shared_ptr<Material>> materials_;
    std::vector<std::shared_ptr<Mesh>>     meshes_;
    std::vector<SceneObject>               objects_;
    UpdateFn                               updateFn_;
};

} // namespace vkr
