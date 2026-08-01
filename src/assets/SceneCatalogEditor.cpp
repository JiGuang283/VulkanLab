#include "SceneCatalogEditor.h"

#include "SceneCatalogStore.h"

namespace vkr {

void SceneCatalogEditor::saveModelPreviewCamera(
    const ProjectContext &project, const std::string &modelId,
    const CameraPose &camera) {
    (void)SceneCatalogStore::updateModelPreviewCamera(project, modelId, camera);
}

void SceneCatalogEditor::removeModel(const ProjectContext &project,
                                     const std::string &modelId) {
    (void)SceneCatalogStore::removeModel(project, modelId);
}

void SceneCatalogEditor::saveCamera(const ProjectContext &project,
                                    const std::string &sceneId,
                                    const CameraPose &camera) {
    saveModelPreviewCamera(project, sceneId, camera);
}

void SceneCatalogEditor::removeScene(const ProjectContext &project,
                                     const std::string &sceneId) {
    removeModel(project, sceneId);
}

void SceneCatalogEditor::addEnvironment(
    const ProjectContext &project,
    const CatalogEnvironment &environment) {
    (void)SceneCatalogStore::addEnvironment(project, environment);
}

void SceneCatalogEditor::removeEnvironment(
    const ProjectContext &project, const std::string &environmentId) {
    (void)SceneCatalogStore::removeEnvironment(project, environmentId);
}

} // namespace vkr
