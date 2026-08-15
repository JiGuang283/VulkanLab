#include "SceneWorkflowController.h"

#include "scene/SceneRegistryBuilder.h"

#include <algorithm>
#include <cctype>

namespace vkr {
namespace {

bool asciiEqualsIgnoreCase(const std::string &left,
                           const std::string &right) {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

SceneWorkflowController::SceneWorkflowController(
    const ProjectContext &projectContext, SceneCatalog catalog)
    : catalog_(std::move(catalog)),
      entries_(buildSceneRegistry(catalog_, projectContext)) {}

void SceneWorkflowController::refresh(
    const ProjectContext &projectContext) {
    catalog_ = SceneCatalog::load(projectContext.catalogPath,
                                  projectContext.projectRoot);
    entries_ = buildSceneRegistry(catalog_, projectContext);
}

int SceneWorkflowController::findEntryByName(const std::string &name) const {
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        if (asciiEqualsIgnoreCase(entries_[index].name, name))
            return index;
    }
    return -1;
}

int SceneWorkflowController::findEntryById(const std::string &id) const {
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        if (asciiEqualsIgnoreCase(entries_[index].id, id))
            return index;
    }
    return -1;
}

SceneWorkflowSnapshot SceneWorkflowController::snapshot() const {
    SceneWorkflowSnapshot result;
    result.projectId = catalog_.projectId;
    result.selectedIndex = assetOperations_.selectedSceneIndex;
    result.models.reserve(catalog_.models.size());
    result.nativeScenes.reserve(catalog_.sceneDocuments.size());
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        const SceneEntry &entry = entries_[index];
        SceneWorkflowItemSnapshot item{index,
                                       entry.id,
                                       entry.name,
                                       entry.sourcePath,
                                       entry.profileId,
                                       entry.available,
                                       entry.unavailableReason};
        if (entry.isNativeScene())
            result.nativeScenes.push_back(std::move(item));
        else
            result.models.push_back(std::move(item));
    }
    return result;
}

} // namespace vkr
