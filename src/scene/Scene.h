#pragma once

#include "render/IRenderWorld.h"
#include "ModelInstance.h"
#include "render/frame/SceneLight.h"
#include "scene_data/SceneTypes.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace vkr {

class MaterialInstance;
struct RenderItem;

// Ready-only facade used by Model Preview entries. GPU resources remain owned
// by shared ModelAssets held through each ModelInstance handle.
class Scene final : public IRenderWorld {
  public:
    size_t addModelInstance(ModelInstance instance);
    bool removeModelInstance(size_t index);

    void collectRenderItems(std::vector<RenderItem> &items) const override;
    RenderWorldFrameSnapshot buildRenderSnapshot() const override;
    void update(float, float) override {}

    const std::vector<ModelInstance> &modelInstances() const {
        return modelInstances_;
    }
    const std::vector<std::shared_ptr<MaterialInstance>> &
    materials() const override {
        return materials_;
    }
    const std::vector<SceneLight> &lights() const override { return lights_; }
    const Bounds &bounds() const override { return bounds_; }
    size_t renderableCount() const override;
    bool allowsFallbackSun() const override { return true; }
    std::optional<CameraPose> initialEditorCamera() const override {
        return initialCamera;
    }
    std::optional<RuntimeCameraView> activeCamera(float) const override {
        return std::nullopt;
    }
    std::optional<RenderWorldAmbient> worldAmbient() const override {
        return std::nullopt;
    }
    std::optional<RenderWorldEnvironment> worldEnvironment() const override {
        return std::nullopt;
    }
    std::optional<RenderWorldAtmosphere> worldAtmosphere() const override {
        return std::nullopt;
    }
    const std::vector<RenderWorldReflectionProbe> &
    reflectionProbes() const override {
        return reflectionProbes_;
    }
    std::optional<RenderWorldDdgiVolume> ddgiProbeVolume() const override {
        return std::nullopt;
    }

    std::optional<CameraPose> initialCamera;

  private:
    std::vector<ModelInstance> modelInstances_;
    std::vector<std::shared_ptr<MaterialInstance>> materials_;
    std::vector<SceneLight> lights_;
    Bounds bounds_;
    std::vector<RenderWorldReflectionProbe> reflectionProbes_;

    void rebuildDerivedState();
};

} // namespace vkr
