#pragma once

#include "ProjectContext.h"
#include "scene/Scene.h"

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
