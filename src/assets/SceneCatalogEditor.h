#pragma once

#include "ProjectContext.h"
#include "SceneCatalog.h"
#include "scene/SceneTypes.h"

#include <filesystem>
#include <string>

namespace vkr {

class SceneCatalogEditor {
  public:
    static void saveCamera(const ProjectContext &project,
                           const std::string &sceneId,
                           const CameraPose &camera);
    static void removeScene(const ProjectContext &project,
                            const std::string &sceneId);
    static void addEnvironment(
        const ProjectContext &project,
        const CatalogEnvironment &environment);
    static void removeEnvironment(const ProjectContext &project,
                                  const std::string &environmentId);
};

} // namespace vkr
