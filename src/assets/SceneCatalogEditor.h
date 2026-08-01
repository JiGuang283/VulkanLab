#pragma once

#include "ProjectContext.h"
#include "SceneCatalog.h"
#include "scene/SceneTypes.h"

#include <filesystem>
#include <string>

namespace vkr {

class SceneCatalogEditor {
  public:
    static void saveModelPreviewCamera(const ProjectContext &project,
                                       const std::string &modelId,
                                       const CameraPose &camera);
    static void removeModel(const ProjectContext &project,
                            const std::string &modelId);

    // Compatibility adapters for the former model-as-scene terminology.
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
