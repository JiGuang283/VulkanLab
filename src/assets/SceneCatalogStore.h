#pragma once

#include "ProjectContext.h"
#include "SceneCatalog.h"

#include <functional>

namespace vkr {

class SceneCatalogStore {
  public:
    using Mutation = std::function<void(SceneCatalog &)>;

    static SceneCatalog load(const ProjectContext &project);
    static SceneCatalog update(const ProjectContext &project,
                               const Mutation &mutation);

    static SceneCatalog addModel(const ProjectContext &project,
                                 CatalogModel model);
    static SceneCatalog removeModel(const ProjectContext &project,
                                    const std::string &modelId);
    static SceneCatalog updateModelPreviewCamera(
        const ProjectContext &project, const std::string &modelId,
        const CameraPose &camera);

    static SceneCatalog addSceneDocument(
        const ProjectContext &project, CatalogSceneDocument scene);
    static SceneCatalog removeSceneDocument(const ProjectContext &project,
                                            const std::string &sceneId);

    static SceneCatalog addEnvironment(
        const ProjectContext &project, CatalogEnvironment environment);
    static SceneCatalog removeEnvironment(
        const ProjectContext &project, const std::string &environmentId);
};

} // namespace vkr
