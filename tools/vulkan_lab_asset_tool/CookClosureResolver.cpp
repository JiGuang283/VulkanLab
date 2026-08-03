#include "CookClosureResolver.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace vkr::assettool {

CookClosure resolveCookClosure(
    const SceneCatalog &catalog, const std::filesystem::path &projectRoot,
    const std::vector<std::string> &requestedSceneDocumentIds,
    const std::optional<std::string> &requestedStartupSceneId) {
    std::unordered_set<std::string> selectedIds(
        requestedSceneDocumentIds.begin(), requestedSceneDocumentIds.end());
    if (selectedIds.size() != requestedSceneDocumentIds.size())
        throw std::invalid_argument("cook scene IDs must be unique");

    CookClosure closure;
    const SceneDocumentReferences references = catalog.documentReferences();
    std::unordered_set<std::string> modelIds;
    std::unordered_set<std::string> environmentIds;

    for (const CatalogSceneDocument &entry : catalog.sceneDocuments) {
        const bool selected = requestedSceneDocumentIds.empty()
                                  ? !entry.optional
                                  : selectedIds.erase(entry.id) > 0;
        if (!selected)
            continue;

        CookSceneRoot root;
        root.catalogEntry = &entry;
        root.loaded = SceneDocumentService::load(
            (projectRoot / entry.source).lexically_normal(), projectRoot,
            &references);
        if (root.loaded.document.id.value() != entry.id) {
            throw std::runtime_error(
                "scene_document_id_mismatch: Catalog scene '" + entry.id +
                "' contains document ID '" +
                root.loaded.document.id.value() + "'");
        }

        std::unordered_set<std::string> sceneModelIds;
        for (const SceneEntityDocument &entity : root.loaded.document.entities) {
            if (!entity.modelInstance)
                continue;
            const std::string &modelId = entity.modelInstance->model.value();
            if (sceneModelIds.insert(modelId).second)
                root.modelIds.push_back(modelId);
            modelIds.insert(modelId);
        }
        if (root.loaded.document.environment) {
            root.environmentId =
                root.loaded.document.environment->environmentId;
            environmentIds.insert(*root.environmentId);
        }
        closure.scenes.push_back(std::move(root));
    }

    if (!selectedIds.empty()) {
        throw std::invalid_argument("unknown native scene ID: " +
                                    *selectedIds.begin());
    }
    if (closure.scenes.empty())
        throw std::invalid_argument("cook selected no native scenes");

    if (requestedStartupSceneId) {
        const bool selected = std::any_of(
            closure.scenes.begin(), closure.scenes.end(),
            [&](const CookSceneRoot &scene) {
                return scene.catalogEntry->id == *requestedStartupSceneId;
            });
        if (!selected) {
            throw std::invalid_argument(
                "startup scene must be one of the selected native scenes: " +
                *requestedStartupSceneId);
        }
        closure.startupSceneId = *requestedStartupSceneId;
    } else {
        closure.startupSceneId = closure.scenes.front().catalogEntry->id;
    }

    std::unordered_set<std::string> importProfileIds;
    importProfileIds.insert(catalog.defaultImportProfile);
    for (const CatalogModel &model : catalog.models) {
        if (modelIds.count(model.id) == 0)
            continue;
        if (model.type == "builtin") {
            throw std::runtime_error(
                "model_not_instanceable: native scene references builtin "
                "model '" + model.id + "'");
        }
        if (model.type != "gltf") {
            throw std::runtime_error("unsupported cooked model type for '" +
                                     model.id + "': " + model.type);
        }
        closure.models.push_back(&model);
        importProfileIds.insert(model.importProfile);
        modelIds.erase(model.id);
    }
    for (const PrimitiveModelDefinition &primitive :
         primitiveModelDefinitions()) {
        if (modelIds.erase(std::string(primitive.id)) > 0)
            closure.primitiveModels.push_back(&primitive);
    }
    if (!modelIds.empty())
        throw std::runtime_error("native scene references unknown model: " +
                                 *modelIds.begin());

    std::vector<std::string> orderedImportProfiles(importProfileIds.begin(),
                                                    importProfileIds.end());
    std::sort(orderedImportProfiles.begin(), orderedImportProfiles.end());
    for (const std::string &profileId : orderedImportProfiles)
        closure.importProfiles.push_back(&catalog.profile(profileId));

    std::unordered_set<std::string> environmentProfileIds;
    for (const CatalogEnvironment &environment : catalog.environments) {
        if (environmentIds.erase(environment.id) == 0)
            continue;
        closure.environments.push_back(&environment);
        environmentProfileIds.insert(environment.environmentProfile);
    }
    if (!environmentIds.empty()) {
        throw std::runtime_error(
            "native scene references unknown environment: " +
            *environmentIds.begin());
    }

    if (environmentProfileIds.empty()) {
        if (catalog.environmentProfiles.empty())
            throw std::runtime_error("Catalog has no environment profiles");
        std::string firstProfile;
        for (const auto &entry : catalog.environmentProfiles) {
            if (firstProfile.empty() || entry.first < firstProfile)
                firstProfile = entry.first;
        }
        environmentProfileIds.insert(std::move(firstProfile));
    }
    std::vector<std::string> orderedEnvironmentProfiles(
        environmentProfileIds.begin(), environmentProfileIds.end());
    std::sort(orderedEnvironmentProfiles.begin(),
              orderedEnvironmentProfiles.end());
    for (const std::string &profileId : orderedEnvironmentProfiles) {
        closure.environmentProfiles.push_back(
            &catalog.environmentProfile(profileId));
    }
    return closure;
}

} // namespace vkr::assettool
