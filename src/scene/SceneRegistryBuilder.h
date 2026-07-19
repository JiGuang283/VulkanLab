#pragma once

#include "SceneFactory.h"

#include <vector>

namespace vkr {

struct Config;
struct ProjectContext;
class SceneCatalog;

std::vector<SceneEntry>
buildSceneRegistry(const SceneCatalog &catalog,
                   const ProjectContext &projectContext,
                   const Config &config);

} // namespace vkr
