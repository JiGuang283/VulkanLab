#include "CookPackageBuilder.h"

#include "CookClosureResolver.h"

#include "GltfValidator.h"
#include "ProcessRunner.h"

#include "assets/AssetValidation.h"
#include "assets/ArtifactIndex.h"
#include "assets/ArtifactStatus.h"
#include "assets/ContentHash.h"
#include "assets/DerivedEnvironmentManifest.h"
#include "assets/DerivedTextureManifest.h"
#include "assets/RuntimePackage.h"
#include "assets/SceneCatalog.h"
#include "assets/ModelImportService.h"
#include "render/shader/ShaderRegistry.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

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
    const ModelImportPreflight preflight =
        ModelImportService::preflight(source);
    const std::set<std::string> buffers = gltfBufferUris(source);
    for (const ModelImportDependency &dependency : preflight.dependencies) {
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
            [&](const ModelImportDependency &dependency) {
                return dependency.uri == uri;
            });
        if (found == preflight.dependencies.end())
            throw std::runtime_error("glTF buffer dependency is unavailable: " +
                                     uri);
    }
}

void saveCookedCatalog(const std::filesystem::path &path,
                       const SceneCatalog &catalog,
                       const CookClosure &closure) {
    Json modelArray = Json::array();
    for (const CatalogModel *model : closure.models) {
        modelArray.push_back({{"id", model->id},
                              {"displayName", model->displayName},
                              {"source", generic(model->source)},
                              {"importProfile", model->importProfile},
                              {"optional", false}});
    }
    Json sceneArray = Json::array();
    for (const CookSceneRoot &scene : closure.scenes) {
        sceneArray.push_back(
            {{"id", scene.catalogEntry->id},
             {"displayName", scene.catalogEntry->displayName},
             {"source", generic(scene.catalogEntry->source)},
             {"optional", false}});
    }

    Json importProfiles = Json::object();
    for (const ImportProfile *profile : closure.importProfiles) {
        importProfiles[profile->id] = {
            {"textureLimit", profile->textureLimit},
            {"textureEncoder", profile->textureEncoder},
            {"qualityPreset", profile->qualityPreset}};
    }
    Json environmentArray = Json::array();
    Json environmentProfiles = Json::object();
    for (const EnvironmentProfile *profile : closure.environmentProfiles) {
        environmentProfiles[profile->id] = {
            {"radianceSize", profile->radianceSize},
            {"irradianceSize", profile->irradianceSize},
            {"prefilteredSize", profile->prefilteredSize},
            {"brdfLutSize", profile->brdfLutSize},
            {"diffuseSamples", profile->diffuseSamples},
            {"specularSamples", profile->specularSamples},
            {"brdfSamples", profile->brdfSamples}};
    }
    for (const CatalogEnvironment *environment : closure.environments) {
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
        {"defaultImportProfile", catalog.defaultImportProfile},
        {"importProfiles", std::move(importProfiles)},
        {"models", std::move(modelArray)},
        {"scenes", std::move(sceneArray)},
        {"environmentProfiles", std::move(environmentProfiles)},
        {"environments", std::move(environmentArray)}};
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not create cooked catalog");
    output << root.dump(2) << '\n';
    if (!output)
        throw std::runtime_error("could not write cooked catalog");
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

struct CookClosureInputSnapshot {
    std::string catalogSha256;
    std::vector<std::pair<std::filesystem::path, std::string>> sceneDocuments;
};

CookClosureInputSnapshot captureClosureInputs(
    const std::filesystem::path &projectRoot, const CookClosure &closure) {
    CookClosureInputSnapshot snapshot;
    snapshot.catalogSha256 =
        sha256File(projectRoot / "assets/catalog.json");
    snapshot.sceneDocuments.reserve(closure.scenes.size());
    for (const CookSceneRoot &scene : closure.scenes) {
        const std::filesystem::path path =
            (projectRoot / scene.catalogEntry->source).lexically_normal();
        snapshot.sceneDocuments.emplace_back(path, sha256File(path));
    }
    return snapshot;
}

void verifyClosureInputsUnchanged(
    const std::filesystem::path &projectRoot,
    const CookClosureInputSnapshot &snapshot) {
    if (sha256File(projectRoot / "assets/catalog.json") !=
        snapshot.catalogSha256) {
        throw std::runtime_error(
            "cook_closure_changed: Catalog changed during cook");
    }
    for (const auto &[path, expectedSha256] : snapshot.sceneDocuments) {
        if (!std::filesystem::is_regular_file(path) ||
            sha256File(path) != expectedSha256) {
            throw std::runtime_error(
                "cook_closure_changed: SceneDocument changed during cook: " +
                path.string());
        }
    }
}

RuntimePackageBuildInfo inspectRuntimeBuild(
    const std::filesystem::path &runtimeExecutable) {
    std::atomic_bool cancelled{false};
    Win32JobProcessRunner runner;
    ProcessRequest request;
    request.executable = runtimeExecutable;
    request.arguments = {L"--build-info-json"};
    request.timeoutMs = 30000;
    request.maxStdoutBytes = 64 * 1024;
    request.maxStderrBytes = 64 * 1024;
    const ProcessResult result = runner.run(request, cancelled);
    if (result.cancelled || result.timedOut || result.stdoutTruncated ||
        result.stderrTruncated || result.exitCode != 0) {
        throw std::runtime_error(
            "could not query VulkanLab runtime build information" +
            (result.stderrText.empty() ? std::string{}
                                       : ": " + result.stderrText));
    }
    try {
        const Json root = Json::parse(result.stdoutText);
        if (root.at("schemaVersion").get<uint32_t>() != 1)
            throw std::runtime_error("unsupported build info schema");
        RuntimePackageBuildInfo build;
        build.revision = root.at("revision").get<std::string>();
        build.configuration =
            root.at("configuration").get<std::string>();
        const Json &features = root.at("features");
        build.editorUi = features.at("editorUi").get<bool>();
        build.runtimeControl = features.at("runtimeControl").get<bool>();
        build.capture = features.at("capture").get<bool>();
        build.assetAuthoring = features.at("assetAuthoring").get<bool>();
        build.validation = features.at("validation").get<bool>();
        build.gpuDebugUtils = features.at("gpuDebugUtils").get<bool>();
        build.gpuProfiling = features.at("gpuProfiling").get<bool>();
        build.tracy = features.at("tracy").get<bool>();
        build.cacao = features.value("cacao", false);
        const Json &rendering = root.at("rendering");
        const std::vector<std::string> renderPaths =
            rendering.at("renderPaths").get<std::vector<std::string>>();
        const std::vector<std::string> materialBindings =
            rendering.at("materialBindings")
                .get<std::vector<std::string>>();
        const auto includes = [](const std::vector<std::string> &values,
                                 std::string_view expected) {
            return std::find(values.begin(), values.end(), expected) !=
                   values.end();
        };
        if (!includes(renderPaths, "forward") ||
            !includes(renderPaths, "deferred")) {
            throw std::runtime_error(
                "runtime build does not contain both render paths");
        }
        if (!includes(materialBindings, "legacy") ||
            !includes(materialBindings, "bindless")) {
            throw std::runtime_error(
                "runtime build does not contain both material binding "
                "backends");
        }
        if (rendering.at("shaderManifestSchema").get<uint32_t>() !=
            ShaderRegistry::kSchemaVersion) {
            throw std::runtime_error(
                "runtime build shader manifest schema is incompatible");
        }
        if (build.configuration != "Release")
            throw std::runtime_error("runtime build must be Release");
        if (build.editorUi || build.runtimeControl || build.capture ||
            build.assetAuthoring || build.validation || build.gpuDebugUtils ||
            build.gpuProfiling || build.tracy || build.cacao) {
            throw std::runtime_error(
                "runtime build contains development-only features; use the "
                "windows-msvc-runtime preset");
        }
        return build;
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "invalid VulkanLab --build-info-json response: " +
            std::string(error.what()));
    }
}

RuntimePackageManifest makePackageManifest(
    const std::filesystem::path &stagingRoot, const SceneCatalog &catalog,
    const CookClosure &closure, const RuntimePackageBuildInfo &runtimeBuild,
    const std::string &platform) {
    RuntimePackageManifest manifest;
    manifest.platform = platform;
    manifest.projectId = catalog.projectId;
    manifest.defaultImportProfile = catalog.defaultImportProfile;
    manifest.profileId = manifest.defaultImportProfile;
    manifest.requiredTextureEncoder = "bc7";
    manifest.startupSceneId = closure.startupSceneId;
    manifest.runtimeBuild = runtimeBuild;
    for (const CookSceneRoot &scene : closure.scenes)
        manifest.sceneIds.push_back(scene.catalogEntry->id);
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
    const CookClosure closure = resolveCookClosure(
        catalog, projectRoot, options.sceneDocumentIds,
        options.startupSceneId);
    const CookClosureInputSnapshot closureInputs =
        captureClosureInputs(projectRoot, closure);
    for (const CatalogModel *model : closure.models) {
        const ImportProfile &profile = catalog.profile(model->importProfile);
        if (profile.textureEncoder != "bc7") {
            throw std::runtime_error(
                "windows-x64 cooked packages require native BC7 import "
                "profiles; '" + profile.id + "' uses " +
                profile.textureEncoder);
        }
    }
    const std::filesystem::path runtimeExecutable =
        runtimeDirectory / "VulkanLab.exe";
    const RuntimePackageBuildInfo runtimeBuild =
        inspectRuntimeBuild(runtimeExecutable);
    const std::filesystem::path staging = uniqueSibling(output, ".staging");
    std::filesystem::create_directories(output.parent_path());
    std::filesystem::create_directories(staging);
    CookPackageReport report;
    report.outputDirectory = output;
    report.sceneCount = closure.scenes.size();
    report.modelCount =
        closure.models.size() + closure.primitiveModels.size();
    std::set<std::string> copiedBlobs;
    try {
        copyFile(runtimeExecutable, staging / "VulkanLab.exe");
        copyShaders(runtimeDirectory, staging);

        for (const CookSceneRoot &scene : closure.scenes) {
            SceneDocumentService::saveAtomic(
                staging / scene.catalogEntry->source, staging,
                scene.loaded.document);
        }

        for (const CatalogModel *model : closure.models) {
            const ImportProfile &profile = catalog.profile(model->importProfile);
            const std::filesystem::path source = projectRoot / model->source;
            const AssetValidationQuery validation = querySceneValidation(
                cacheRoot, projectRoot, model->id);
            if (!validation.report ||
                (validation.state != AssetValidationState::Valid &&
                 validation.state != AssetValidationState::Warnings) ||
                validation.report->validatorVersion !=
                    kGltfValidatorVersion) {
                throw std::runtime_error(
                    "validation is not current for '" + model->id +
                    "': " + assetValidationStateName(validation.state) +
                    (validation.reason.empty()
                         ? std::string{}
                         : " (" + validation.reason + ")"));
            }
            const ArtifactStatus status = inspectTextureArtifacts(
                {cacheRoot, source, catalog.projectId, model->id, profile.id,
                 profile.textureLimit, TextureEncoder::Bc7});
            if (!status.ready())
                throw std::runtime_error("artifacts are not Ready for '" +
                                         model->id + "': " + status.reason);
            copySceneGeometry(projectRoot, *model, staging);

            DerivedTextureManifest manifest;
            std::string error;
            const std::filesystem::path sourceManifest =
                derivedManifestPath(cacheRoot, model->id, profile.id);
            if (!loadDerivedTextureManifest(sourceManifest, manifest, error))
                throw std::runtime_error("could not load artifact manifest: " +
                                         error);
            const std::filesystem::path cookedScene = staging / model->source;
            manifest.scenePath = generic(model->source);
            manifest.scene = fileStamp(cookedScene, sha256File(cookedScene));
            manifest.scene.path = model->source.filename().generic_string();
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
                derivedManifestPath(staging / "runtime_assets", model->id,
                                    profile.id);
            if (!saveDerivedTextureManifest(cookedManifest, manifest, error))
                throw std::runtime_error(
                    "could not write cooked texture manifest: " + error);
            ++report.manifestCount;
        }

        for (const CatalogEnvironment *environment : closure.environments) {
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

        saveCookedCatalog(staging / "assets/catalog.json", catalog, closure);
        const SceneCatalog cookedCatalog = SceneCatalog::load(
            staging / "assets/catalog.json", staging);
        ArtifactIndex index = ArtifactIndex::rebuild(
            staging / "runtime_assets", staging, cookedCatalog);
        index.save();

        RuntimePackageManifest package = makePackageManifest(
            staging, cookedCatalog, closure, runtimeBuild, options.platform);
        std::string error;
        if (!saveRuntimePackageManifest(staging / "package_manifest.json",
                                        package, error)) {
            throw std::runtime_error("could not write package manifest: " +
                                     error);
        }
        const RuntimePackageVerification verified =
            verifyRuntimePackage(staging, package);
        verifyClosureInputsUnchanged(projectRoot, closureInputs);
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
