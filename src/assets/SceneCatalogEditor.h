#pragma once

#include "ProjectContext.h"
#include "scene/SceneTypes.h"

#include <string>

namespace vkr {

class SceneCatalogEditor {
  public:
    static void saveCamera(const ProjectContext &project,
                           const std::string &sceneId,
                           const CameraPose &camera);
    static void removeScene(const ProjectContext &project,
                            const std::string &sceneId);
};

} // namespace vkr
