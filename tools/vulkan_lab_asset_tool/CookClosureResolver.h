#pragma once

#include "assets/SceneCatalog.h"
#include "scene_data/SceneDocument.h"
#include "scene_data/PrimitiveModelDefinitions.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr::assettool {

struct CookSceneRoot {
    const CatalogSceneDocument *catalogEntry = nullptr;
    LoadedSceneDocument loaded;
    std::vector<std::string> modelIds;
    std::vector<std::string> environmentIds;
};

struct CookClosure {
    std::vector<CookSceneRoot> scenes;
    std::vector<const CatalogModel *> models;
    std::vector<const PrimitiveModelDefinition *> primitiveModels;
    std::vector<const CatalogEnvironment *> environments;
    std::vector<const ImportProfile *> importProfiles;
    std::vector<const EnvironmentProfile *> environmentProfiles;
    std::string startupSceneId;
};

CookClosure resolveCookClosure(
    const SceneCatalog &catalog, const std::filesystem::path &projectRoot,
    const std::vector<std::string> &requestedSceneDocumentIds,
    const std::optional<std::string> &requestedStartupSceneId);

} // namespace vkr::assettool
