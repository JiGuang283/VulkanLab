#include "SceneCatalogStore.h"

#include "AssetValidation.h"
#include "DerivedTextureManifest.h"
#include "scene_data/SceneDocument.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace vkr {
namespace {

using Json = nlohmann::ordered_json;

bool sameStamp(const DerivedFileStamp &left, const DerivedFileStamp &right) {
    return left.size == right.size && left.writeTime == right.writeTime;
}

Json cameraJson(const CameraPose &camera) {
    return {{"position",
             {camera.position.x, camera.position.y, camera.position.z}},
            {"yaw", camera.yaw},
            {"pitch", camera.pitch}};
}

template <typename Map>
std::vector<std::string> sortedKeys(const Map &map) {
    std::vector<std::string> result;
    result.reserve(map.size());
    for (const auto &[key, value] : map) {
        (void)value;
        result.push_back(key);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Json serializeCatalog(const SceneCatalog &catalog) {
    Json root = Json::object();
    root["schemaVersion"] = SceneCatalog::kSchemaVersion;
    root["projectId"] = catalog.projectId;
    root["defaultImportProfile"] = catalog.defaultImportProfile;

    root["importProfiles"] = Json::object();
    for (const std::string &id : sortedKeys(catalog.importProfiles)) {
        const ImportProfile &profile = catalog.importProfiles.at(id);
        root["importProfiles"][id] = {
            {"textureLimit", profile.textureLimit},
            {"textureEncoder", profile.textureEncoder},
            {"qualityPreset", profile.qualityPreset}};
    }

    root["environmentProfiles"] = Json::object();
    for (const std::string &id : sortedKeys(catalog.environmentProfiles)) {
        const EnvironmentProfile &profile =
            catalog.environmentProfiles.at(id);
        root["environmentProfiles"][id] = {
            {"radianceSize", profile.radianceSize},
            {"irradianceSize", profile.irradianceSize},
            {"prefilteredSize", profile.prefilteredSize},
            {"brdfLutSize", profile.brdfLutSize},
            {"diffuseSamples", profile.diffuseSamples},
            {"specularSamples", profile.specularSamples},
            {"brdfSamples", profile.brdfSamples}};
    }
    root["defaultEnvironment"] =
        catalog.defaultEnvironment ? Json(*catalog.defaultEnvironment)
                                   : Json(nullptr);

    root["models"] = Json::array();
    for (const CatalogModel &model : catalog.models) {
        Json item = {{"id", model.id},
                     {"displayName", model.displayName},
                     {"type", model.type},
                     {"importProfile", model.importProfile},
                     {"optional", model.optional}};
        if (model.type == "builtin")
            item["builtinFactory"] = model.builtinFactory;
        else
            item["source"] = model.source.generic_string();
        if (model.previewCamera)
            item["previewCamera"] = cameraJson(*model.previewCamera);
        root["models"].push_back(std::move(item));
    }

    root["scenes"] = Json::array();
    for (const CatalogSceneDocument &scene : catalog.sceneDocuments) {
        root["scenes"].push_back({{"id", scene.id},
                                  {"displayName", scene.displayName},
                                  {"source", scene.source.generic_string()},
                                  {"optional", scene.optional}});
    }

    root["environments"] = Json::array();
    for (const CatalogEnvironment &environment : catalog.environments) {
        root["environments"].push_back(
            {{"id", environment.id},
             {"displayName", environment.displayName},
             {"source", environment.source.generic_string()},
             {"environmentProfile", environment.environmentProfile},
             {"optional", environment.optional}});
    }
    return root;
}

std::filesystem::path temporaryCatalogPath(
    const std::filesystem::path &catalogPath) {
    return catalogPath.string() + ".edit-" +
           std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()) +
           ".tmp";
}

void atomicReplace(const std::filesystem::path &temporary,
                   const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not atomically replace Catalog (error " +
                                 std::to_string(GetLastError()) + ")");
    }
}

void validateSceneDocuments(const ProjectContext &project,
                            const SceneCatalog &catalog) {
    const SceneDocumentReferences references = catalog.documentReferences();
    for (const CatalogSceneDocument &scene : catalog.sceneDocuments) {
        const std::filesystem::path source =
            project.resolveProjectPath(scene.source);
        if (scene.optional && !std::filesystem::is_regular_file(source))
            continue;
        const LoadedSceneDocument loaded = SceneDocumentService::load(
            source, project.projectRoot, &references);
        if (loaded.document.id.value() != scene.id) {
            throw std::runtime_error(
                "Scene document ID does not match its Catalog entry: " +
                scene.id);
        }
    }
}

} // namespace

SceneCatalog SceneCatalogStore::load(const ProjectContext &project) {
    return SceneCatalog::load(project.catalogPath, project.projectRoot);
}

SceneCatalog SceneCatalogStore::update(const ProjectContext &project,
                                       const Mutation &mutation) {
    if (!project.catalogWritable)
        throw std::runtime_error("The project Catalog is read-only");
    const DerivedFileStamp before = fileStamp(project.catalogPath);
    SceneCatalog catalog = load(project);
    mutation(catalog);
    catalog.schemaVersion = SceneCatalog::kSchemaVersion;
    validateSceneDocuments(project, catalog);

    const std::filesystem::path temporary =
        temporaryCatalogPath(project.catalogPath);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not create temporary Catalog");
        output << serializeCatalog(catalog).dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not flush temporary Catalog");
        output.close();

        SceneCatalog checked =
            SceneCatalog::load(temporary, project.projectRoot);
        if (!sameStamp(before, fileStamp(project.catalogPath))) {
            throw std::runtime_error(
                "Catalog changed during edit; refusing to overwrite it");
        }
        atomicReplace(temporary, project.catalogPath);
        return checked;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

SceneCatalog SceneCatalogStore::addModel(const ProjectContext &project,
                                         CatalogModel model) {
    return update(project, [model = std::move(model)](SceneCatalog &catalog) {
        if (catalog.findModel(model.id) || catalog.findSceneDocument(model.id))
            throw std::runtime_error("Asset ID already exists: " + model.id);
        catalog.models.push_back(model);
    });
}

SceneCatalog SceneCatalogStore::removeModel(const ProjectContext &project,
                                            const std::string &modelId) {
    SceneCatalog result = update(project, [&](SceneCatalog &catalog) {
        const auto found =
            std::find_if(catalog.models.begin(), catalog.models.end(),
                         [&](const CatalogModel &model) {
                             return model.id == modelId;
                         });
        if (found == catalog.models.end())
            throw std::runtime_error("Model is not present in Catalog: " +
                                     modelId);
        catalog.models.erase(found);
    });
    removeSceneValidationBinding(project.cacheRoot, modelId);
    return result;
}

SceneCatalog SceneCatalogStore::updateModelPreviewCamera(
    const ProjectContext &project, const std::string &modelId,
    const CameraPose &camera) {
    return update(project, [&](SceneCatalog &catalog) {
        CatalogModel *model = nullptr;
        for (CatalogModel &candidate : catalog.models) {
            if (candidate.id == modelId) {
                model = &candidate;
                break;
            }
        }
        if (!model)
            throw std::runtime_error("Model is not present in Catalog: " +
                                     modelId);
        model->previewCamera = camera;
    });
}

SceneCatalog SceneCatalogStore::addSceneDocument(
    const ProjectContext &project, CatalogSceneDocument scene) {
    return update(project, [scene = std::move(scene)](SceneCatalog &catalog) {
        if (catalog.findModel(scene.id) || catalog.findSceneDocument(scene.id))
            throw std::runtime_error("Asset ID already exists: " + scene.id);
        catalog.sceneDocuments.push_back(scene);
    });
}

SceneCatalog SceneCatalogStore::removeSceneDocument(
    const ProjectContext &project, const std::string &sceneId) {
    return update(project, [&](SceneCatalog &catalog) {
        const auto found = std::find_if(
            catalog.sceneDocuments.begin(), catalog.sceneDocuments.end(),
            [&](const CatalogSceneDocument &scene) {
                return scene.id == sceneId;
            });
        if (found == catalog.sceneDocuments.end())
            throw std::runtime_error(
                "Scene document is not present in Catalog: " + sceneId);
        catalog.sceneDocuments.erase(found);
    });
}

SceneCatalog SceneCatalogStore::addEnvironment(
    const ProjectContext &project, CatalogEnvironment environment) {
    return update(project, [environment = std::move(environment)](
                               SceneCatalog &catalog) {
        if (catalog.findEnvironment(environment.id))
            throw std::runtime_error(
                "Environment is already present in Catalog: " +
                environment.id);
        catalog.environments.push_back(environment);
    });
}

SceneCatalog SceneCatalogStore::removeEnvironment(
    const ProjectContext &project, const std::string &environmentId) {
    return update(project, [&](SceneCatalog &catalog) {
        const auto found = std::find_if(
            catalog.environments.begin(), catalog.environments.end(),
            [&](const CatalogEnvironment &environment) {
                return environment.id == environmentId;
            });
        if (found == catalog.environments.end())
            throw std::runtime_error(
                "Environment is not present in Catalog: " + environmentId);
        catalog.environments.erase(found);
        if (catalog.defaultEnvironment == environmentId)
            catalog.defaultEnvironment.reset();
    });
}

} // namespace vkr
