#pragma once

#include "EditorDockWorkspace.h"
#include "scene_data/SceneIds.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>

namespace vkr {

class SceneEditorSession;

enum class GizmoOperation { Select, Translate, Rotate, Scale };
enum class GizmoSpace { Local, World };

struct SceneViewportCamera {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 1.0f, 0.0f};
    bool cameraDragging = false;
};

struct SceneViewportActions {
    std::function<void(const std::string &, const glm::vec3 &)>
        instantiateModel;
    std::function<std::string(const std::string &)> modelDisplayName;
    std::function<void(std::string)> reportError;
};

class SceneViewportController {
  public:
    struct Ray {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 1.0f, 0.0f};
    };

    void beginFrame();
    void drawToolbar();
    void drawOverlay(const EditorViewportState &viewport,
                     const SceneViewportCamera &camera,
                     SceneEditorSession &session,
                     const SceneViewportActions &actions);

    bool manipulationActive() const { return manipulationActive_; }
    bool blocksViewportInput() const;
    void cancelManipulation();

  private:
    static std::optional<Ray>
    viewportRay(const EditorViewportState &viewport,
                const SceneViewportCamera &camera, const glm::vec2 &point);

    GizmoOperation operation_ = GizmoOperation::Translate;
    GizmoSpace space_ = GizmoSpace::World;
    bool manipulationActive_ = false;
    bool manipulationChanged_ = false;
    bool pointerOverGizmo_ = false;
    bool dragDropActive_ = false;
    std::optional<PersistentEntityId> manipulatedEntity_;
    SceneEditorSession *activeSession_ = nullptr;
};

} // namespace vkr
