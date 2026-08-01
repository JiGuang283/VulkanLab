#include "SceneCatalog.h"

#include "scene_data/SceneDocument.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace vkr {
namespace {

using Json = nlohmann::json;

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

std::runtime_error fieldError(const std::string &field,
                              const std::string &message) {
    return std::runtime_error("Invalid catalog field '" + field + "': " +
                              message);
}

std::optional<CameraPose> parseCamera(const Json &model,
                                      const std::string &field) {
    const char *key = model.contains("previewCamera") ? "previewCamera"
                                                      : "camera";
    if (!model.contains(key))
        return std::nullopt;
    try {
        const Json &camera = model.at(key);
        const auto position = camera.at("position").get<std::vector<float>>();
        if (position.size() != 3)
            throw fieldError(field + ".position", "expected three numbers");
        CameraPose pose{{position[0], position[1], position[2]},
                        camera.at("yaw").get<float>(),
                        camera.at("pitch").get<float>()};
        if (!std::isfinite(pose.position.x) ||
            !std::isfinite(pose.position.y) ||
            !std::isfinite(pose.position.z) || !std::isfinite(pose.yaw) ||
            !std::isfinite(pose.pitch)) {
            throw fieldError(field, "camera values must be finite");
        }
        return pose;
    } catch (const std::runtime_error &) {
        throw;
    } catch (const std::exception &exception) {
        throw fieldError(field, exception.what());
    }
}

void validateProjectRelativePath(const std::filesystem::path &projectRoot,
                                 const std::filesystem::path &source,
                                 const std::string &field) {
    if (source.empty() || source.is_absolute())
        throw fieldError(field, "must be a project-relative path");
    if (!pathIsWithin(projectRoot, projectRoot / source))
        throw fieldError(field, "path escapes the project root");
}

bool hasSceneDocumentSuffix(const std::filesystem::path &path) {
    const std::string filename = asciiLower(path.filename().string());
    constexpr const char *suffix = ".vkscene.json";
    constexpr size_t suffixLength = 13;
    return filename.size() >= suffixLength &&
           filename.compare(filename.size() - suffixLength, suffixLength,
                            suffix) == 0;
}

void parseImportProfiles(const Json &root, SceneCatalog &catalog) {
    const Json &profiles = root.at("importProfiles");
    if (!profiles.is_object() || profiles.empty())
        throw fieldError("importProfiles", "expected a non-empty object");
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        ImportProfile profile;
        profile.id = it.key();
        if (!isStableAssetId(profile.id))
            throw fieldError("importProfiles." + profile.id,
                             "invalid profile ID");
        profile.textureLimit = it.value().at("textureLimit").get<uint32_t>();
        if (profile.textureLimit != 0 && profile.textureLimit != 512 &&
            profile.textureLimit != 1024 && profile.textureLimit != 2048) {
            throw fieldError("importProfiles." + profile.id +
                                 ".textureLimit",
                             "expected 0, 512, 1024, or 2048");
        }
        profile.textureEncoder =
            it.value().value("textureEncoder", std::string("uastc"));
        if (profile.textureEncoder != "uastc" &&
            profile.textureEncoder != "bc7") {
            throw fieldError("importProfiles." + profile.id +
                                 ".textureEncoder",
                             "expected uastc or bc7");
        }
        profile.qualityPreset = it.value().value(
            "qualityPreset", std::string("development"));
        catalog.importProfiles.emplace(profile.id, std::move(profile));
    }
    if (catalog.importProfiles.count(catalog.defaultImportProfile) == 0)
        throw fieldError("defaultImportProfile", "unknown profile ID");
}

void addDefaultEnvironmentProfile(SceneCatalog &catalog) {
    EnvironmentProfile profile;
    profile.id = "ibl_desktop_v1";
    catalog.environmentProfiles.emplace(profile.id, std::move(profile));
}

void parseEnvironmentProfiles(const Json &root, SceneCatalog &catalog) {
    const Json &profiles = root.at("environmentProfiles");
    if (!profiles.is_object() || profiles.empty())
        throw fieldError("environmentProfiles", "expected a non-empty object");
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        EnvironmentProfile profile;
        profile.id = it.key();
        if (!isStableAssetId(profile.id))
            throw fieldError("environmentProfiles." + profile.id,
                             "invalid profile ID");
        const Json &value = it.value();
        profile.radianceSize = value.value("radianceSize", profile.radianceSize);
        profile.irradianceSize =
            value.value("irradianceSize", profile.irradianceSize);
        profile.prefilteredSize =
            value.value("prefilteredSize", profile.prefilteredSize);
        profile.brdfLutSize = value.value("brdfLutSize", profile.brdfLutSize);
        profile.diffuseSamples =
            value.value("diffuseSamples", profile.diffuseSamples);
        profile.specularSamples =
            value.value("specularSamples", profile.specularSamples);
        profile.brdfSamples = value.value("brdfSamples", profile.brdfSamples);
        const auto validSize = [](uint32_t size) {
            return size > 0 && (size & (size - 1)) == 0 && size <= 4096;
        };
        if (!validSize(profile.radianceSize) ||
            !validSize(profile.irradianceSize) ||
            !validSize(profile.prefilteredSize) ||
            !validSize(profile.brdfLutSize)) {
            throw fieldError("environmentProfiles." + profile.id,
                             "sizes must be powers of two <= 4096");
        }
        if (profile.diffuseSamples == 0 || profile.specularSamples == 0 ||
            profile.brdfSamples == 0) {
            throw fieldError("environmentProfiles." + profile.id,
                             "sample counts must be non-zero");
        }
        catalog.environmentProfiles.emplace(profile.id, std::move(profile));
    }
}

void parseEnvironments(const Json &root,
                       const std::filesystem::path &projectRoot,
                       SceneCatalog &catalog) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> displayNames;
    const Json environments = root.value("environments", Json::array());
    if (!environments.is_array())
        throw fieldError("environments", "expected an array");
    for (size_t index = 0; index < environments.size(); ++index) {
        const Json &item = environments.at(index);
        const std::string field =
            "environments[" + std::to_string(index) + "]";
        CatalogEnvironment environment;
        environment.id = item.at("id").get<std::string>();
        environment.displayName = item.at("displayName").get<std::string>();
        environment.environmentProfile = item.value(
            "environmentProfile", std::string("ibl_desktop_v1"));
        environment.optional = item.value("optional", false);
        if (!isStableAssetId(environment.id))
            throw fieldError(field + ".id", "invalid stable ID");
        if (!ids.insert(environment.id).second)
            throw fieldError(field + ".id", "duplicate environment ID");
        if (environment.displayName.empty())
            throw fieldError(field + ".displayName", "cannot be empty");
        if (!displayNames.insert(asciiLower(environment.displayName)).second)
            throw fieldError(field + ".displayName", "duplicate display name");
        if (catalog.environmentProfiles.count(environment.environmentProfile) == 0)
            throw fieldError(field + ".environmentProfile", "unknown profile ID");
        environment.source = std::filesystem::path(
                                 item.at("source").get<std::string>())
                                 .lexically_normal();
        validateProjectRelativePath(projectRoot, environment.source,
                                    field + ".source");
        if (asciiLower(environment.source.extension().string()) != ".hdr")
            throw fieldError(field + ".source", "expected a .hdr file");
        if (!environment.optional &&
            !std::filesystem::is_regular_file(projectRoot / environment.source))
            throw fieldError(field + ".source", "file is missing");
        catalog.environments.push_back(std::move(environment));
    }
}

CatalogModel parseModel(const Json &item, const std::string &field,
                        const std::filesystem::path &projectRoot,
                        const SceneCatalog &catalog) {
    CatalogModel model;
    model.id = item.at("id").get<std::string>();
    model.displayName = item.at("displayName").get<std::string>();
    model.type = item.value("type", std::string("gltf"));
    model.builtinFactory = item.value("builtinFactory", std::string{});
    model.importProfile =
        item.value("importProfile", catalog.defaultImportProfile);
    model.optional = item.value("optional", false);
    model.previewCamera = parseCamera(item, field + ".previewCamera");

    if (!isStableAssetId(model.id))
        throw fieldError(field + ".id", "invalid stable ID");
    if (model.displayName.empty())
        throw fieldError(field + ".displayName", "cannot be empty");
    if (catalog.importProfiles.count(model.importProfile) == 0)
        throw fieldError(field + ".importProfile", "unknown profile ID");
    if (model.type == "builtin") {
        if (model.builtinFactory.empty())
            throw fieldError(field + ".builtinFactory",
                             "cannot be empty for builtin models");
    } else if (model.type == "gltf") {
        model.source = std::filesystem::path(
                           item.at("source").get<std::string>())
                           .lexically_normal();
        validateProjectRelativePath(projectRoot, model.source,
                                    field + ".source");
        const std::string extension =
            asciiLower(model.source.extension().string());
        if (extension != ".gltf" && extension != ".glb")
            throw fieldError(field + ".source", "expected a .gltf or .glb file");
        if (!model.optional &&
            !std::filesystem::is_regular_file(projectRoot / model.source))
            throw fieldError(field + ".source", "file is missing");
    } else {
        throw fieldError(field + ".type", "unknown model type");
    }
    return model;
}

void parseModels(const Json &items, const std::string &arrayField,
                 const std::filesystem::path &projectRoot,
                 SceneCatalog &catalog,
                 std::unordered_set<std::string> &allIds) {
    if (!items.is_array())
        throw fieldError(arrayField, "expected an array");
    std::unordered_set<std::string> displayNames;
    for (size_t index = 0; index < items.size(); ++index) {
        const std::string field =
            arrayField + "[" + std::to_string(index) + "]";
        CatalogModel model =
            parseModel(items.at(index), field, projectRoot, catalog);
        if (!allIds.insert(model.id).second)
            throw fieldError(field + ".id", "duplicate asset ID");
        if (!displayNames.insert(asciiLower(model.displayName)).second)
            throw fieldError(field + ".displayName", "duplicate display name");
        catalog.models.push_back(std::move(model));
    }
}

void parseSceneDocuments(const Json &items,
                         const std::filesystem::path &projectRoot,
                         SceneCatalog &catalog,
                         std::unordered_set<std::string> &allIds) {
    if (!items.is_array())
        throw fieldError("scenes", "expected an array");
    std::unordered_set<std::string> displayNames;
    for (size_t index = 0; index < items.size(); ++index) {
        const Json &item = items.at(index);
        const std::string field = "scenes[" + std::to_string(index) + "]";
        CatalogSceneDocument scene;
        scene.id = item.at("id").get<std::string>();
        scene.displayName = item.at("displayName").get<std::string>();
        scene.optional = item.value("optional", false);
        scene.source = std::filesystem::path(
                           item.at("source").get<std::string>())
                           .lexically_normal();
        if (!isStableAssetId(scene.id))
            throw fieldError(field + ".id", "invalid stable ID");
        if (!allIds.insert(scene.id).second)
            throw fieldError(field + ".id", "duplicate asset ID");
        if (scene.displayName.empty())
            throw fieldError(field + ".displayName", "cannot be empty");
        if (!displayNames.insert(asciiLower(scene.displayName)).second)
            throw fieldError(field + ".displayName", "duplicate display name");
        validateProjectRelativePath(projectRoot, scene.source,
                                    field + ".source");
        if (!hasSceneDocumentSuffix(scene.source))
            throw fieldError(field + ".source",
                             "expected a .vkscene.json file");
        if (!scene.optional &&
            !std::filesystem::is_regular_file(projectRoot / scene.source))
            throw fieldError(field + ".source", "file is missing");
        catalog.sceneDocuments.push_back(std::move(scene));
    }
}

} // namespace

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
    std::error_code error;
    const auto normalizedRoot =
        std::filesystem::weakly_canonical(root, error).lexically_normal();
    if (error)
        return false;
    error.clear();
    auto normalizedCandidate =
        std::filesystem::weakly_canonical(candidate, error).lexically_normal();
    if (error)
        normalizedCandidate =
            std::filesystem::absolute(candidate, error).lexically_normal();
    if (error)
        return false;
    auto rootIt = normalizedRoot.begin();
    auto candidateIt = normalizedCandidate.begin();
    for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == normalizedCandidate.end())
            return false;
#ifdef _WIN32
        if (asciiLower(rootIt->string()) != asciiLower(candidateIt->string()))
#else
        if (*rootIt != *candidateIt)
#endif
            return false;
    }
    return true;
}

SceneCatalog SceneCatalog::load(const std::filesystem::path &catalogPath,
                                const std::filesystem::path &projectRoot) {
    try {
        std::ifstream input(catalogPath, std::ios::binary);
        if (!input)
            throw std::runtime_error("could not open file");
        Json root;
        input >> root;

        SceneCatalog catalog;
        catalog.schemaVersion = root.at("schemaVersion").get<uint32_t>();
        if (catalog.schemaVersion != kLegacySchemaVersion &&
            catalog.schemaVersion != kEnvironmentSchemaVersion &&
            catalog.schemaVersion != kSchemaVersion) {
            throw fieldError("schemaVersion", "unsupported schema");
        }
        catalog.projectId = root.at("projectId").get<std::string>();
        if (!isStableAssetId(catalog.projectId))
            throw fieldError("projectId", "expected a stable lowercase ID");
        catalog.defaultImportProfile =
            root.at("defaultImportProfile").get<std::string>();
        parseImportProfiles(root, catalog);

        if (catalog.schemaVersion >= kEnvironmentSchemaVersion) {
            parseEnvironmentProfiles(root, catalog);
            if (root.contains("defaultEnvironment") &&
                !root.at("defaultEnvironment").is_null()) {
                catalog.defaultEnvironment =
                    root.at("defaultEnvironment").get<std::string>();
            }
            parseEnvironments(root, projectRoot, catalog);
            if (catalog.defaultEnvironment &&
                !catalog.findEnvironment(*catalog.defaultEnvironment)) {
                throw fieldError("defaultEnvironment",
                                 "unknown environment ID");
            }
        } else {
            addDefaultEnvironmentProfile(catalog);
        }

        std::unordered_set<std::string> assetIds;
        if (catalog.schemaVersion >= kSchemaVersion) {
            parseModels(root.at("models"), "models", projectRoot, catalog,
                        assetIds);
            parseSceneDocuments(root.at("scenes"), projectRoot, catalog,
                                assetIds);
        } else {
            // In v1/v2, `scenes` represented imported models and their preview.
            parseModels(root.at("scenes"), "scenes", projectRoot, catalog,
                        assetIds);
        }
        return catalog;
    } catch (const std::runtime_error &) {
        throw;
    } catch (const std::exception &exception) {
        throw std::runtime_error("Could not load scene catalog '" +
                                 catalogPath.string() + "': " +
                                 exception.what());
    }
}

const ImportProfile &SceneCatalog::profile(const std::string &id) const {
    const auto it = importProfiles.find(id);
    if (it == importProfiles.end())
        throw std::out_of_range("Unknown import profile: " + id);
    return it->second;
}

const EnvironmentProfile &
SceneCatalog::environmentProfile(const std::string &id) const {
    const auto it = environmentProfiles.find(id);
    if (it == environmentProfiles.end())
        throw std::out_of_range("Unknown environment profile: " + id);
    return it->second;
}

const CatalogModel *SceneCatalog::findModel(const std::string &id) const {
    const auto it = std::find_if(models.begin(), models.end(),
                                 [&](const CatalogModel &model) {
                                     return model.id == id;
                                 });
    return it == models.end() ? nullptr : &*it;
}

const CatalogSceneDocument *
SceneCatalog::findSceneDocument(const std::string &id) const {
    const auto it =
        std::find_if(sceneDocuments.begin(), sceneDocuments.end(),
                     [&](const CatalogSceneDocument &scene) {
                         return scene.id == id;
                     });
    return it == sceneDocuments.end() ? nullptr : &*it;
}

const CatalogEnvironment *
SceneCatalog::findEnvironment(const std::string &id) const {
    const auto it =
        std::find_if(environments.begin(), environments.end(),
                     [&](const CatalogEnvironment &environment) {
                         return environment.id == id;
                     });
    return it == environments.end() ? nullptr : &*it;
}

SceneDocumentReferences SceneCatalog::documentReferences() const {
    SceneDocumentReferences references;
    for (const CatalogModel &model : models)
        references.modelIds.insert(model.id);
    for (const CatalogEnvironment &environment : environments)
        references.environmentIds.insert(environment.id);
    return references;
}

} // namespace vkr
