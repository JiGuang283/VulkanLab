#pragma once

#include "SceneTypes.h"
#include "scene_data/SceneDocument.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

class MaterialInstance;
class RenderQueue;
struct SceneLight;

struct RuntimeCameraView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
    float nearPlane = 0.05f;
    float farPlane = 1000.0f;
};

struct RenderWorldAmbient {
    glm::vec3 color{1.0f};
    float intensity = 0.08f;
};

struct RenderWorldEnvironment {
    std::string id;
    float intensity = 1.0f;
    float rotationRadians = 0.0f;
};

struct RenderWorldAtmosphere {
    PersistentEntityId entityId;
    glm::vec3 groundOriginWS{0.0f};
    AtmosphereComponentDocument parameters{};
};

class IRenderWorld {
  public:
    virtual ~IRenderWorld() = default;

    virtual void update(float dt, float time) = 0;
    virtual void collectRenderCommands(RenderQueue &queue) const = 0;
    virtual const Bounds &bounds() const = 0;
    virtual const std::vector<SceneLight> &lights() const = 0;
    virtual const std::vector<std::shared_ptr<MaterialInstance>> &
    materials() const = 0;
    virtual size_t renderableCount() const = 0;
    virtual bool allowsFallbackSun() const = 0;
    virtual std::optional<CameraPose> initialEditorCamera() const = 0;
    virtual std::optional<RuntimeCameraView>
    activeCamera(float aspect) const = 0;
    virtual std::optional<RenderWorldAmbient> worldAmbient() const = 0;
    virtual std::optional<RenderWorldEnvironment>
    worldEnvironment() const = 0;
    virtual std::optional<RenderWorldAtmosphere>
    worldAtmosphere() const = 0;
};

} // namespace vkr
