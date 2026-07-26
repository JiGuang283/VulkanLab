#include "SceneCatalog.h"

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
        const unsigned char byte = static_cast<unsigned char>(c);
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

std::runtime_error fieldError(const std::string &field,
                              const std::string &message) {
    return std::runtime_error("Invalid catalog field '" + field + "': " +
                              message);
}

std::optional<CameraPose> parseCamera(const Json &scene,
                                      const std::string &field) {
    if (!scene.contains("camera"))
        return std::nullopt;
    try {
        const Json &camera = scene.at("camera");
        const auto position = camera.at("position").get<std::vector<float>>();
        if (position.size() != 3)
            throw fieldError(field + ".position", "expected three numbers");
        CameraPose pose{{position[0], position[1], position[2]},
                        camera.at("yaw").get<float>(),
                        camera.at("pitch").get<float>()};
        if (!std::isfinite(pose.position.x) ||
            !std::isfinite(pose.position.y) ||
            !std::isfinite(pose.position.z) || !std::isfinite(pose.yaw) ||
            !std::isfinite(pose.pitch))
            throw fieldError(field, "camera values must be finite");
        return pose;
    } catch (const std::runtime_error &) {
        throw;
    } catch (const std::exception &exception) {
        throw fieldError(field, exception.what());
    }
}

} // namespace

bool isStableAssetId(const std::string &value) {
    if (value.empty())
        return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') ||
                           (i > 0 && (c == '_' || c == '-'));
        if (!valid)
            return false;
    }
    return true;
}

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
            catalog.schemaVersion != kSchemaVersion)
            throw fieldError("schemaVersion", "unsupported schema");
        catalog.projectId = root.at("projectId").get<std::string>();
        if (!isStableAssetId(catalog.projectId))
            throw fieldError("projectId", "expected a stable lowercase ID");
        catalog.defaultImportProfile =
            root.at("defaultImportProfile").get<std::string>();

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
                profile.textureLimit != 1024 && profile.textureLimit != 2048)
                throw fieldError("importProfiles." + profile.id +
                                     ".textureLimit",
                                 "expected 0, 512, 1024, or 2048");
            profile.textureEncoder =
                it.value().value("textureEncoder", std::string("uastc"));
            profile.qualityPreset = it.value().value(
                "qualityPreset", std::string("development"));
            catalog.importProfiles.emplace(profile.id, std::move(profile));
        }
        if (catalog.importProfiles.find(catalog.defaultImportProfile) ==
            catalog.importProfiles.end())
            throw fieldError("defaultImportProfile", "unknown profile ID");

        if (catalog.schemaVersion >= kSchemaVersion) {
            const Json &environmentProfiles =
                root.at("environmentProfiles");
            if (!environmentProfiles.is_object() ||
                environmentProfiles.empty()) {
                throw fieldError("environmentProfiles",
                                 "expected a non-empty object");
            }
            for (auto it = environmentProfiles.begin();
                 it != environmentProfiles.end(); ++it) {
                EnvironmentProfile profile;
                profile.id = it.key();
                if (!isStableAssetId(profile.id)) {
                    throw fieldError("environmentProfiles." + profile.id,
                                     "invalid profile ID");
                }
                const Json &value = it.value();
                profile.radianceSize =
                    value.value("radianceSize", profile.radianceSize);
                profile.irradianceSize =
                    value.value("irradianceSize", profile.irradianceSize);
                profile.prefilteredSize =
                    value.value("prefilteredSize", profile.prefilteredSize);
                profile.brdfLutSize =
                    value.value("brdfLutSize", profile.brdfLutSize);
                profile.diffuseSamples =
                    value.value("diffuseSamples", profile.diffuseSamples);
                profile.specularSamples =
                    value.value("specularSamples", profile.specularSamples);
                profile.brdfSamples =
                    value.value("brdfSamples", profile.brdfSamples);
                const auto validSize = [](uint32_t size) {
                    return size > 0 && (size & (size - 1)) == 0 &&
                           size <= 4096;
                };
                if (!validSize(profile.radianceSize) ||
                    !validSize(profile.irradianceSize) ||
                    !validSize(profile.prefilteredSize) ||
                    !validSize(profile.brdfLutSize)) {
                    throw fieldError("environmentProfiles." + profile.id,
                                     "sizes must be powers of two <= 4096");
                }
                if (profile.diffuseSamples == 0 ||
                    profile.specularSamples == 0 ||
                    profile.brdfSamples == 0) {
                    throw fieldError("environmentProfiles." + profile.id,
                                     "sample counts must be non-zero");
                }
                catalog.environmentProfiles.emplace(profile.id,
                                                    std::move(profile));
            }

            if (root.contains("defaultEnvironment") &&
                !root.at("defaultEnvironment").is_null()) {
                catalog.defaultEnvironment =
                    root.at("defaultEnvironment").get<std::string>();
            }

            std::unordered_set<std::string> environmentIds;
            std::unordered_set<std::string> environmentDisplayNames;
            const Json environments =
                root.value("environments", Json::array());
            if (!environments.is_array()) {
                throw fieldError("environments", "expected an array");
            }
            for (size_t index = 0; index < environments.size(); ++index) {
                const Json &item = environments[index];
                const std::string field =
                    "environments[" + std::to_string(index) + "]";
                CatalogEnvironment environment;
                environment.id = item.at("id").get<std::string>();
                environment.displayName =
                    item.at("displayName").get<std::string>();
                environment.environmentProfile = item.value(
                    "environmentProfile", std::string("ibl_desktop_v1"));
                environment.optional = item.value("optional", false);
                if (!isStableAssetId(environment.id))
                    throw fieldError(field + ".id", "invalid stable ID");
                if (!environmentIds.insert(environment.id).second)
                    throw fieldError(field + ".id",
                                     "duplicate environment ID");
                if (environment.displayName.empty())
                    throw fieldError(field + ".displayName",
                                     "cannot be empty");
                if (!environmentDisplayNames
                         .insert(asciiLower(environment.displayName))
                         .second) {
                    throw fieldError(field + ".displayName",
                                     "duplicate display name");
                }
                if (catalog.environmentProfiles.count(
                        environment.environmentProfile) == 0) {
                    throw fieldError(field + ".environmentProfile",
                                     "unknown profile ID");
                }
                const std::string source =
                    item.at("source").get<std::string>();
                environment.source =
                    std::filesystem::path(source).lexically_normal();
                if (environment.source.empty() ||
                    environment.source.is_absolute()) {
                    throw fieldError(field + ".source",
                                     "must be a project-relative path");
                }
                const std::filesystem::path resolved =
                    projectRoot / environment.source;
                if (!pathIsWithin(projectRoot, resolved))
                    throw fieldError(field + ".source",
                                     "path escapes the project root");
                if (asciiLower(environment.source.extension().string()) !=
                    ".hdr") {
                    throw fieldError(field + ".source",
                                     "expected a .hdr file");
                }
                if (!environment.optional &&
                    !std::filesystem::is_regular_file(resolved)) {
                    throw fieldError(field + ".source", "file is missing");
                }
                catalog.environments.push_back(std::move(environment));
            }
            if (catalog.defaultEnvironment &&
                !catalog.findEnvironment(*catalog.defaultEnvironment)) {
                throw fieldError("defaultEnvironment",
                                 "unknown environment ID");
            }
        } else {
            EnvironmentProfile profile;
            profile.id = "ibl_desktop_v1";
            catalog.environmentProfiles.emplace(profile.id,
                                                std::move(profile));
        }

        std::unordered_set<std::string> ids;
        std::unordered_set<std::string> displayNames;
        const Json &scenes = root.at("scenes");
        if (!scenes.is_array() || scenes.empty())
            throw fieldError("scenes", "expected a non-empty array");
        for (size_t index = 0; index < scenes.size(); ++index) {
            const Json &item = scenes[index];
            const std::string field = "scenes[" + std::to_string(index) + "]";
            CatalogScene scene;
            scene.id = item.at("id").get<std::string>();
            scene.displayName = item.at("displayName").get<std::string>();
            scene.type = item.value("type", std::string("gltf"));
            scene.builtinFactory =
                item.value("builtinFactory", std::string{});
            scene.importProfile = item.value(
                "importProfile", catalog.defaultImportProfile);
            scene.optional = item.value("optional", false);
            scene.camera = parseCamera(item, field + ".camera");

            if (!isStableAssetId(scene.id))
                throw fieldError(field + ".id", "invalid stable ID");
            if (!ids.insert(scene.id).second)
                throw fieldError(field + ".id", "duplicate scene ID");
            if (scene.displayName.empty())
                throw fieldError(field + ".displayName", "cannot be empty");
            if (!displayNames.insert(asciiLower(scene.displayName)).second)
                throw fieldError(field + ".displayName",
                                 "duplicate display name");
            if (catalog.importProfiles.find(scene.importProfile) ==
                catalog.importProfiles.end())
                throw fieldError(field + ".importProfile",
                                 "unknown profile ID");

            if (scene.type == "builtin") {
                if (scene.builtinFactory.empty())
                    throw fieldError(field + ".builtinFactory",
                                     "cannot be empty for builtin scenes");
            } else if (scene.type == "gltf") {
                const std::string source = item.at("source").get<std::string>();
                scene.source = std::filesystem::path(source).lexically_normal();
                if (scene.source.empty() || scene.source.is_absolute())
                    throw fieldError(field + ".source",
                                     "must be a project-relative path");
                const std::filesystem::path resolved =
                    projectRoot / scene.source;
                if (!pathIsWithin(projectRoot, resolved))
                    throw fieldError(field + ".source",
                                     "path escapes the project root");
                const std::string extension =
                    asciiLower(scene.source.extension().string());
                if (extension != ".gltf" && extension != ".glb")
                    throw fieldError(field + ".source",
                                     "expected a .gltf or .glb file");
                if (!scene.optional &&
                    !std::filesystem::is_regular_file(resolved))
                    throw fieldError(field + ".source", "file is missing");
            } else {
                throw fieldError(field + ".type", "unknown scene type");
            }
            catalog.scenes.push_back(std::move(scene));
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

const CatalogScene *SceneCatalog::findScene(const std::string &id) const {
    const auto it = std::find_if(scenes.begin(), scenes.end(),
                                 [&](const CatalogScene &scene) {
                                     return scene.id == id;
                                 });
    return it == scenes.end() ? nullptr : &*it;
}

const CatalogEnvironment *
SceneCatalog::findEnvironment(const std::string &id) const {
    const auto it = std::find_if(
        environments.begin(), environments.end(),
        [&](const CatalogEnvironment &environment) {
            return environment.id == id;
        });
    return it == environments.end() ? nullptr : &*it;
}

} // namespace vkr
