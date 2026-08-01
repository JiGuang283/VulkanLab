#include "CookPackageBuilder.h"

#include "GltfValidator.h"

#include "assets/AssetValidation.h"
#include "assets/ArtifactIndex.h"
#include "assets/ArtifactStatus.h"
#include "assets/ContentHash.h"
#include "assets/DerivedEnvironmentManifest.h"
#include "assets/DerivedTextureManifest.h"
#include "assets/RuntimePackage.h"
#include "assets/SceneCatalog.h"
#include "assets/SceneImportService.h"
#include "render/ShaderRegistry.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace vkr::assettool {
namespace {

using Json = nlohmann::json;

std::string generic(const std::filesystem::path &path) {
    return path.lexically_normal().generic_string();
}

void copyFile(const std::filesystem::path &source,
              const std::filesystem::path &destination) {
    if (!std::filesystem::is_regular_file(source))
        throw std::runtime_error("cook input file is missing: " +
                                 source.string());
    std::filesystem::create_directories(destination.parent_path());
    std::error_code error;
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        error);
    if (error)
        throw std::runtime_error("could not copy '" + source.string() +
                                 "': " + error.message());
}

std::set<std::string> gltfBufferUris(const std::filesystem::path &scene) {
    std::set<std::string> result;
    if (scene.extension() != ".gltf")
        return result;
    std::ifstream input(scene, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not read glTF for cook: " +
                                 scene.string());
    Json root;
    input >> root;
    if (!root.contains("buffers"))
        return result;
    for (const Json &buffer : root.at("buffers")) {
        const std::string uri = buffer.value("uri", std::string{});
        if (!uri.empty() && uri.rfind("data:", 0) != 0)
            result.insert(uri);
    }
    return result;
}

void copySceneGeometry(const std::filesystem::path &projectRoot,
                       const CatalogModel &scene,
                       const std::filesystem::path &stagingRoot) {
    const std::filesystem::path source = projectRoot / scene.source;
    copyFile(source, stagingRoot / scene.source);
    if (scene.source.extension() != ".gltf")
        return;
    const SceneImportPreflight preflight =
        ModelImportService::preflight(source);
    const std::set<std::string> buffers = gltfBufferUris(source);
    for (const SceneImportDependency &dependency : preflight.dependencies) {
        if (buffers.count(dependency.uri) == 0)
            continue;
        copyFile(dependency.sourcePath,
                 (stagingRoot / scene.source.parent_path() /
                  dependency.relativePath)
                     .lexically_normal());
    }
    for (const std::string &uri : buffers) {
        const auto found = std::find_if(
            preflight.dependencies.begin(), preflight.dependencies.end(),
            [&](const SceneImportDependency &dependency) {
                return dependency.uri == uri;
            });
        if (found == preflight.dependencies.end())
            throw std::runtime_error("glTF buffer dependency is unavailable: " +
                                     uri);
    }
}

Json cameraJson(const CameraPose &camera) {
    return {{"position", {camera.position.x, camera.position.y,
                           camera.position.z}},
            {"yaw", camera.yaw},
            {"pitch", camera.pitch}};
}

void saveCookedCatalog(const std::filesystem::path &path,
                       const SceneCatalog &catalog,
                       const std::vector<const CatalogModel *> &scenes,
                       const std::vector<const CatalogEnvironment *> &environments,
                       const ImportProfile &profile) {
    Json sceneArray = Json::array();
    for (const CatalogModel *scene : scenes) {
        Json item = {{"id", scene->id},
                     {"displayName", scene->displayName},
                     {"importProfile", profile.id}};
        if (scene->type == "builtin") {
            item["type"] = "builtin";
            item["builtinFactory"] = scene->builtinFactory;
        } else {
            item["source"] = generic(scene->source);
        }
        if (scene->previewCamera)
            item["previewCamera"] = cameraJson(*scene->previewCamera);
        sceneArray.push_back(std::move(item));
    }
    Json environmentArray = Json::array();
    Json environmentProfiles = Json::object();
    for (const auto &entry : catalog.environmentProfiles) {
        const EnvironmentProfile &environmentProfile = entry.second;
        environmentProfiles[environmentProfile.id] = {
            {"radianceSize", environmentProfile.radianceSize},
            {"irradianceSize", environmentProfile.irradianceSize},
            {"prefilteredSize", environmentProfile.prefilteredSize},
            {"brdfLutSize", environmentProfile.brdfLutSize},
            {"diffuseSamples", environmentProfile.diffuseSamples},
            {"specularSamples", environmentProfile.specularSamples},
            {"brdfSamples", environmentProfile.brdfSamples}};
    }
    for (const CatalogEnvironment *environment : environments) {
        environmentArray.push_back(
            {{"id", environment->id},
             {"displayName", environment->displayName},
             {"source", generic(environment->source)},
             {"environmentProfile",
              environment->environmentProfile},
             // Cooked packages intentionally omit source HDR files. The
             // runtime consumes only the packaged KTX2 manifest and blobs.
             {"optional", true}});
    }
    Json root = {
        {"schemaVersion", SceneCatalog::kSchemaVersion},
        {"projectId", catalog.projectId},
        {"defaultImportProfile", profile.id},
        {"importProfiles",
         {{profile.id,
           {{"textureLimit", profile.textureLimit},
            {"textureEncoder", profile.textureEncoder},
            {"qualityPreset", profile.qualityPreset}}}}},
        {"models", std::move(sceneArray)},
        {"scenes", Json::array()},
        {"environmentProfiles", std::move(environmentProfiles)},
        {"environments", std::move(environmentArray)}};
    if (catalog.defaultEnvironment &&
        std::any_of(environments.begin(), environments.end(),
                    [&](const CatalogEnvironment *environment) {
                        return environment->id ==
                               *catalog.defaultEnvironment;
                    })) {
        root["defaultEnvironment"] = *catalog.defaultEnvironment;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not create cooked catalog");
    output << root.dump(2) << '\n';
    if (!output)
        throw std::runtime_error("could not write cooked catalog");
}

std::vector<const CatalogModel *>
selectScenes(const SceneCatalog &catalog,
             const std::vector<std::string> &requested) {
    std::unordered_set<std::string> selectedIds(requested.begin(),
                                                requested.end());
    if (selectedIds.size() != requested.size())
        throw std::invalid_argument("cook scene IDs must be unique");
    std::vector<const CatalogModel *> result;
    for (const CatalogModel &scene : catalog.models) {
        const bool selected = requested.empty() ? !scene.optional
                                                : selectedIds.erase(scene.id) > 0;
        if (selected)
            result.push_back(&scene);
    }
    if (!selectedIds.empty())
        throw std::invalid_argument("unknown cook scene ID: " +
                                    *selectedIds.begin());
    if (result.empty())
        throw std::invalid_argument("cook selected no scenes");
    return result;
}

std::vector<const CatalogEnvironment *> selectEnvironments(
    const SceneCatalog &catalog,
    const std::vector<std::string> &requested) {
    std::unordered_set<std::string> selectedIds(requested.begin(),
                                                requested.end());
    if (selectedIds.size() != requested.size()) {
        throw std::invalid_argument(
            "cook environment IDs must be unique");
    }
    std::vector<const CatalogEnvironment *> result;
    for (const CatalogEnvironment &environment :
         catalog.environments) {
        const bool selected =
            requested.empty() ||
            selectedIds.erase(environment.id) > 0;
        if (selected)
            result.push_back(&environment);
    }
    if (!selectedIds.empty()) {
        throw std::invalid_argument(
            "unknown cook environment ID: " +
            *selectedIds.begin());
    }
    return result;
}

void copyShaders(const std::filesystem::path &runtimeDirectory,
                 const std::filesystem::path &stagingRoot) {
    const std::filesystem::path manifest =
        (runtimeDirectory / "shader/manifest.json").lexically_normal();
    const ShaderRegistry registry = ShaderRegistry::load(manifest);
    for (const std::filesystem::path &source : registry.spirvPaths()) {
        if (!pathIsWithin(runtimeDirectory, source))
            throw std::runtime_error("shader path escapes runtime directory: " +
                                     source.string());
        const std::filesystem::path relative =
            source.lexically_relative(runtimeDirectory);
        copyFile(source, stagingRoot / relative);
    }
    copyFile(manifest, stagingRoot / "shader/manifest.json");
}

bool pathsOverlap(const std::filesystem::path &left,
                  const std::filesystem::path &right) {
    return pathIsWithin(left, right) || pathIsWithin(right, left);
}

RuntimePackageManifest makePackageManifest(
    const std::filesystem::path &stagingRoot, const SceneCatalog &catalog,
    const ImportProfile &profile, const std::string &platform) {
    RuntimePackageManifest manifest;
    manifest.platform = platform;
    manifest.projectId = catalog.projectId;
    manifest.profileId = profile.id;
    manifest.requiredTextureEncoder = "bc7";
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             stagingRoot,
             std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         it != end; it.increment(error)) {
        if (error)
            throw std::runtime_error("could not enumerate cooked package: " +
                                     error.message());
        if (!it->is_regular_file() ||
            it->path().filename() == "package_manifest.json")
            continue;
        const std::string relative = generic(
            std::filesystem::relative(it->path(), stagingRoot));
        manifest.files.push_back(
            {relative, it->file_size(), sha256File(it->path())});
    }
    std::sort(manifest.files.begin(), manifest.files.end(),
              [](const auto &left, const auto &right) {
                  return left.path < right.path;
              });
    return manifest;
}

std::filesystem::path uniqueSibling(const std::filesystem::path &output,
                                    const char *suffix) {
    return output.parent_path() /
           (output.filename().string() + suffix + "-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
}

void publishDirectory(const std::filesystem::path &staging,
                      const std::filesystem::path &output) {
    const std::filesystem::path backup = uniqueSibling(output, ".backup");
    const bool hadOutput = std::filesystem::exists(output);
    if (hadOutput)
        std::filesystem::rename(output, backup);
    try {
        std::filesystem::rename(staging, output);
    } catch (...) {
        if (hadOutput && !std::filesystem::exists(output))
            std::filesystem::rename(backup, output);
        throw;
    }
    if (hadOutput) {
        std::error_code ignored;
        std::filesystem::remove_all(backup, ignored);
    }
}

} // namespace

CookPackageReport buildCookPackage(const CookPackageOptions &options) {
    if (options.platform != "windows-x64")
        throw std::invalid_argument("cook supports only windows-x64");
    if (options.profileId.empty())
        throw std::invalid_argument("cook profile is required");
    const std::filesystem::path projectRoot =
        std::filesystem::absolute(options.projectRoot).lexically_normal();
    const std::filesystem::path cacheRoot =
        std::filesystem::absolute(options.cacheRoot).lexically_normal();
    const std::filesystem::path runtimeDirectory =
        std::filesystem::absolute(options.runtimeDirectory).lexically_normal();
    const std::filesystem::path output =
        std::filesystem::absolute(options.outputDirectory).lexically_normal();
    const bool outputContainsProject = pathIsWithin(output, projectRoot);
    if (outputContainsProject || pathsOverlap(output, cacheRoot) ||
        pathsOverlap(output, runtimeDirectory)) {
        throw std::invalid_argument("cook output overlaps an input root");
    }

    const SceneCatalog catalog =
        SceneCatalog::load(projectRoot / "assets/catalog.json", projectRoot);
    const ImportProfile &profile = catalog.profile(options.profileId);
    if (profile.textureEncoder != "bc7") {
        throw std::runtime_error(
            "windows-x64 cooked packages require a native BC7 import profile");
    }
    const std::vector<const CatalogModel *> scenes =
        selectScenes(catalog, options.sceneIds);
    const std::vector<const CatalogEnvironment *> environments =
        selectEnvironments(catalog, options.environmentIds);
    const std::filesystem::path staging = uniqueSibling(output, ".staging");
    std::filesystem::create_directories(output.parent_path());
    std::filesystem::create_directories(staging);
    CookPackageReport report;
    report.outputDirectory = output;
    std::set<std::string> copiedBlobs;
    try {
        copyFile(runtimeDirectory / "VulkanLab.exe",
                 staging / "VulkanLab.exe");
        if (std::filesystem::is_regular_file(runtimeDirectory /
                                             "VulkanLabCtl.exe")) {
            copyFile(runtimeDirectory / "VulkanLabCtl.exe",
                     staging / "VulkanLabCtl.exe");
        }
        copyShaders(runtimeDirectory, staging);

        for (const CatalogModel *scene : scenes) {
            ++report.sceneCount;
            if (scene->type == "builtin") {
                if (scene->builtinFactory != "viking_room")
                    throw std::runtime_error(
                        "unsupported cooked builtin scene: " + scene->id);
                copyFile(projectRoot / "models/viking_room.obj",
                         staging / "models/viking_room.obj");
                copyFile(projectRoot / "textures/viking_room.png",
                         staging / "textures/viking_room.png");
                continue;
            }

            const std::filesystem::path source = projectRoot / scene->source;
            const AssetValidationQuery validation = querySceneValidation(
                cacheRoot, projectRoot, scene->id);
            if (!validation.report ||
                (validation.state != AssetValidationState::Valid &&
                 validation.state != AssetValidationState::Warnings) ||
                validation.report->validatorVersion !=
                    kGltfValidatorVersion) {
                throw std::runtime_error(
                    "validation is not current for '" + scene->id +
                    "': " + assetValidationStateName(validation.state) +
                    (validation.reason.empty()
                         ? std::string{}
                         : " (" + validation.reason + ")"));
            }
            const ArtifactStatus status = inspectTextureArtifacts(
                {cacheRoot, source, catalog.projectId, scene->id, profile.id,
                 profile.textureLimit, TextureEncoder::Bc7});
            if (!status.ready())
                throw std::runtime_error("artifacts are not Ready for '" +
                                         scene->id + "': " + status.reason);
            copySceneGeometry(projectRoot, *scene, staging);

            DerivedTextureManifest manifest;
            std::string error;
            const std::filesystem::path sourceManifest =
                derivedManifestPath(cacheRoot, scene->id, profile.id);
            if (!loadDerivedTextureManifest(sourceManifest, manifest, error))
                throw std::runtime_error("could not load artifact manifest: " +
                                         error);
            const std::filesystem::path cookedScene = staging / scene->source;
            manifest.scenePath = generic(scene->source);
            manifest.scene = fileStamp(cookedScene, sha256File(cookedScene));
            manifest.scene.path = scene->source.filename().generic_string();
            for (DerivedTextureEntry &entry : manifest.entries) {
                const std::filesystem::path sourceBlob = cacheRoot / entry.blob;
                const std::filesystem::path cookedBlob =
                    staging / "runtime_assets" / entry.blob;
                if (copiedBlobs.insert(entry.blob).second) {
                    copyFile(sourceBlob, cookedBlob);
                    ++report.blobCount;
                }
                entry.source = manifest.scene;
            }
            const std::filesystem::path cookedManifest =
                derivedManifestPath(staging / "runtime_assets", scene->id,
                                    profile.id);
            if (!saveDerivedTextureManifest(cookedManifest, manifest, error))
                throw std::runtime_error(
                    "could not write cooked texture manifest: " + error);
            ++report.manifestCount;
        }

        for (const CatalogEnvironment *environment : environments) {
            const std::filesystem::path source =
                projectRoot / environment->source;
            const ArtifactStatus status = inspectEnvironmentArtifacts(
                {cacheRoot, source, catalog.projectId, environment->id,
                 environment->environmentProfile});
            if (!status.ready()) {
                throw std::runtime_error(
                    "environment artifacts are not Ready for '" +
                    environment->id + "': " + status.reason);
            }
            DerivedEnvironmentManifest manifest;
            std::string error;
            const std::filesystem::path sourceManifest =
                derivedEnvironmentManifestPath(
                    cacheRoot, environment->id,
                    environment->environmentProfile);
            if (!loadDerivedEnvironmentManifest(
                    sourceManifest, manifest, error)) {
                throw std::runtime_error(
                    "could not load environment manifest: " +
                    error);
            }
            for (DerivedEnvironmentImage &image : manifest.images) {
                const std::filesystem::path sourceBlob =
                    cacheRoot / image.blob;
                const std::filesystem::path cookedBlob =
                    staging / "runtime_assets" / image.blob;
                if (copiedBlobs.insert(image.blob).second) {
                    copyFile(sourceBlob, cookedBlob);
                    ++report.blobCount;
                }
            }
            manifest.source = {};
            const std::filesystem::path cookedManifest =
                derivedEnvironmentManifestPath(
                    staging / "runtime_assets", environment->id,
                    environment->environmentProfile);
            if (!saveDerivedEnvironmentManifest(
                    cookedManifest, manifest, error)) {
                throw std::runtime_error(
                    "could not write cooked environment manifest: " +
                    error);
            }
            ++report.environmentCount;
            ++report.manifestCount;
        }

        saveCookedCatalog(staging / "assets/catalog.json", catalog, scenes,
                          environments, profile);
        const SceneCatalog cookedCatalog = SceneCatalog::load(
            staging / "assets/catalog.json", staging);
        ArtifactIndex index = ArtifactIndex::rebuild(
            staging / "runtime_assets", staging, cookedCatalog);
        index.save();

        RuntimePackageManifest package = makePackageManifest(
            staging, cookedCatalog, profile, options.platform);
        std::string error;
        if (!saveRuntimePackageManifest(staging / "package_manifest.json",
                                        package, error)) {
            throw std::runtime_error("could not write package manifest: " +
                                     error);
        }
        const RuntimePackageVerification verified =
            verifyRuntimePackage(staging, package);
        report.fileCount = verified.fileCount;
        report.totalBytes = verified.totalBytes;
        publishDirectory(staging, output);
        return report;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
}

} // namespace vkr::assettool
