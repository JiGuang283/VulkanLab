#pragma once

#include "SceneLight.h"
#include "ModelInstance.h"
#include "SceneObject.h"
#include "SceneTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace vkr {

class Mesh;
class MaterialInstance;
class MaterialTemplate;
class RenderQueue;
class Texture;

class Scene {
  public:
    using UpdateFn = std::function<void(Scene &, float dt, float time)>;

    // ---- 资源装填（由场景工厂在构造期调用） ----
    void addTexture(std::shared_ptr<Texture> t) {
        textures_.push_back(std::move(t));
    }
    void addMaterial(std::shared_ptr<MaterialInstance> m) {
        legacyMaterials_.push_back(std::move(m));
        rebuildDerivedState();
    }
    void addMaterialTemplate(std::shared_ptr<MaterialTemplate> t) {
        materialTemplates_.push_back(std::move(t));
    }
    void addMesh(std::shared_ptr<Mesh> m) { meshes_.push_back(std::move(m)); }
    void addObject(SceneObject obj);
    void addLight(SceneLight light) {
        legacyLights_.push_back(std::move(light));
        rebuildDerivedState();
    }
    size_t addModelInstance(ModelInstance instance);
    bool removeModelInstance(size_t index);

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
    const std::vector<ModelInstance> &modelInstances() const {
        return modelInstances_;
    }
    const std::vector<std::shared_ptr<MaterialInstance>> &materials() const {
        return materials_;
    }
    std::vector<SceneLight>        &lights() { return lights_; }
    const std::vector<SceneLight>  &lights() const { return lights_; }
    const Bounds                   &bounds() const { return bounds_; }
    size_t renderableCount() const;
    std::optional<CameraPose> initialCamera;

  private:
    std::vector<std::shared_ptr<Texture>>  textures_;
    std::vector<std::shared_ptr<MaterialTemplate>> materialTemplates_;
    std::vector<std::shared_ptr<MaterialInstance>> legacyMaterials_;
    std::vector<std::shared_ptr<MaterialInstance>> materials_;
    std::vector<std::shared_ptr<Mesh>>     meshes_;
    std::vector<SceneObject>               objects_;
    std::vector<ModelInstance>             modelInstances_;
    std::vector<SceneLight>                legacyLights_;
    std::vector<SceneLight>                lights_;
    Bounds                                 bounds_;
    UpdateFn                               updateFn_;

    void rebuildDerivedState();
};

} // namespace vkr
