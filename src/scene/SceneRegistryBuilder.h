#pragma once

#include "SceneEntry.h"

#include <vector>

namespace vkr {

struct ProjectContext;
class SceneCatalog;

std::vector<SceneEntry>
buildSceneRegistry(const SceneCatalog &catalog,
                   const ProjectContext &projectContext);

} // namespace vkr
