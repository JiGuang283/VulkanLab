#include "RuntimePackage.h"

#include "ArtifactIndex.h"
#include "ArtifactStatus.h"
#include "ContentHash.h"
#include "DerivedEnvironmentManifest.h"
#include "DerivedTextureManifest.h"
#include "SceneCatalog.h"
#include "scene_data/PrimitiveModelDefinitions.h"
#include "scene_data/SceneDocument.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace vkr {
namespace {

using Json = nlohmann::json;

bool isSafeRelativePath(const std::string &value) {
    if (value.empty())
        return false;
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto &component : path) {
        if (component == "..")
            return false;
    }
    return value == path.lexically_normal().generic_string();
}

bool isSha256(const std::string &value) {
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

int hexDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

std::string decodeLocalUriPath(const std::string &uri) {
    if (uri.find('\\') != std::string::npos)
        throw std::runtime_error("backslashes are not allowed in glTF URI: " +
                                 uri);
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] != '%') {
            decoded.push_back(uri[index]);
            continue;
        }
        if (index + 2 >= uri.size())
            throw std::runtime_error("invalid percent escape in glTF URI: " +
                                     uri);
        const int high = hexDigit(uri[index + 1]);
        const int low = hexDigit(uri[index + 2]);
        if (high < 0 || low < 0)
            throw std::runtime_error("invalid percent escape in glTF URI: " +
                                     uri);
        const char value = static_cast<char>((high << 4) | low);
        if (value == '\0')
            throw std::runtime_error("NUL is not allowed in a glTF URI");
        decoded.push_back(value);
        index += 2;
    }
    return decoded;
}

std::filesystem::path requirePackageFile(
    const std::filesystem::path &root, const std::filesystem::path &relative,
    const std::string &description) {
    const std::string normalized = relative.lexically_normal().generic_string();
    if (!isSafeRelativePath(normalized))
        throw std::runtime_error(description + " path is unsafe: " + normalized);
    const std::filesystem::path path = (root / relative).lexically_normal();
    if (!pathIsWithin(root, path) || !std::filesystem::is_regular_file(path))
        throw std::runtime_error(description + " is missing: " + normalized);
    return path;
}

void verifyGltfBuffers(const std::filesystem::path &packageRoot,
                       const CatalogModel &model) {
    if (model.source.extension() != ".gltf")
        return;
    const std::filesystem::path source =
        requirePackageFile(packageRoot, model.source, "model source");
    std::ifstream input(source, std::ios::binary);
    Json root;
    input >> root;
    for (const Json &buffer : root.value("buffers", Json::array())) {
        const std::string uri = buffer.value("uri", std::string{});
        if (uri.empty() || uri.rfind("data:", 0) == 0)
            continue;
        const size_t colon = uri.find(':');
        const size_t slash = uri.find_first_of("/\\");
        if (colon != std::string::npos &&
            (slash == std::string::npos || colon < slash))
            throw std::runtime_error("remote glTF buffer is not packageable: " +
                                     uri);
        requirePackageFile(
            packageRoot,
            model.source.parent_path() /
                std::filesystem::path(decodeLocalUriPath(uri)),
            "glTF buffer");
    }
}

void verifyShaderClosure(const std::filesystem::path &packageRoot) {
    const std::filesystem::path manifestPath = requirePackageFile(
        packageRoot, "shader/manifest.json", "shader manifest");
    std::ifstream input(manifestPath, std::ios::binary);
    Json manifest;
    input >> manifest;
    for (const Json &program : manifest.at("programs")) {
        for (const char *stage : {"vertex", "fragment", "compute"}) {
            if (!program.contains(stage))
                continue;
            std::filesystem::path relative =
                std::filesystem::path("shader") /
                program.at(stage).get<std::string>();
            relative += ".spv";
            requirePackageFile(packageRoot, relative, "shader binary");
        }
    }
}

void verifyArtifactIndexClosure(
    const std::filesystem::path &packageRoot,
    const RuntimePackageManifest &package, const SceneCatalog &catalog) {
    const std::filesystem::path indexPath = requirePackageFile(
        packageRoot, std::filesystem::path(package.cacheRoot) /
                         "artifact_index.json",
        "artifact index");
    std::ifstream input(indexPath, std::ios::binary);
    Json root;
    input >> root;
    if (root.at("schemaVersion").get<uint32_t>() !=
        ArtifactIndex::kSchemaVersion) {
        throw std::runtime_error("artifact index schema mismatch");
    }
    if (root.at("projectId").get<std::string>() != catalog.projectId)
        throw std::runtime_error("artifact index project ID mismatch");

    std::set<std::tuple<std::string, std::string, std::string>> expected;
    for (const CatalogModel &model : catalog.models)
        expected.emplace("Model", model.id, model.importProfile);
    for (const CatalogEnvironment &environment : catalog.environments) {
        expected.emplace("Environment", environment.id,
                         environment.environmentProfile);
    }
    std::unordered_map<
        std::string,
        std::set<std::tuple<std::string, std::string, std::string>>>
        expectedSceneReferences;
    const SceneDocumentReferences documentReferences =
        catalog.documentReferences();
    for (const CatalogSceneDocument &scene : catalog.sceneDocuments) {
        expected.emplace("SceneDocument", scene.id, std::string{});
        const LoadedSceneDocument loaded = SceneDocumentService::load(
            packageRoot / scene.source, packageRoot, &documentReferences);
        auto &references = expectedSceneReferences[scene.id];
        for (const SceneEntityDocument &entity : loaded.document.entities) {
            if (!entity.modelInstance)
                continue;
            const CatalogModel *model = catalog.findModel(
                entity.modelInstance->model.value());
            if (!model &&
                isPrimitiveModelId(entity.modelInstance->model.value())) {
                continue;
            }
            if (!model)
                throw std::runtime_error(
                    "artifact index scene references an unknown model");
            references.emplace("Model", model->id, model->importProfile);
        }
        if (loaded.document.environment) {
            const CatalogEnvironment *environment = catalog.findEnvironment(
                loaded.document.environment->environmentId);
            if (!environment)
                throw std::runtime_error(
                    "artifact index scene references an unknown environment");
            references.emplace("Environment", environment->id,
                               environment->environmentProfile);
        }
    }

    std::set<std::tuple<std::string, std::string, std::string>> actual;
    for (const Json &record : root.at("records")) {
        const std::string kind = record.at("assetKind").get<std::string>();
        const std::string normalizedKind = kind == "Scene" ? "Model" : kind;
        if (record.at("state").get<std::string>() != "Ready") {
            throw std::runtime_error("artifact index record is not Ready: " +
                                     record.at("assetId").get<std::string>());
        }
        actual.emplace(normalizedKind,
                       record.at("assetId").get<std::string>(),
                       record.value("profileId", std::string{}));
        if (normalizedKind != "SceneDocument")
            continue;

        const std::string sceneId =
            record.at("assetId").get<std::string>();
        std::set<std::tuple<std::string, std::string, std::string>>
            actualReferences;
        for (const Json &reference :
             record.value("assetReferences", Json::array())) {
            std::string referenceKind =
                reference.at("assetKind").get<std::string>();
            if (referenceKind == "Scene")
                referenceKind = "Model";
            const auto inserted = actualReferences.emplace(
                referenceKind,
                reference.at("assetId").get<std::string>(),
                reference.value("profileId", std::string{}));
            if (!inserted.second) {
                throw std::runtime_error(
                    "artifact index contains duplicate scene asset references");
            }
        }
        const auto expectedReferences = expectedSceneReferences.find(sceneId);
        if (expectedReferences == expectedSceneReferences.end() ||
            actualReferences != expectedReferences->second) {
            throw std::runtime_error(
                "artifact index SceneDocument references do not match the "
                "packaged scene closure: " +
                sceneId);
        }
    }
    if (actual != expected)
        throw std::runtime_error(
            "artifact index does not match the packaged asset closure");
}

void verifyNativeSceneClosure(const std::filesystem::path &packageRoot,
                              const RuntimePackageManifest &package) {
    const SceneCatalog catalog = SceneCatalog::load(
        packageRoot / package.catalogPath, packageRoot);
    if (catalog.defaultImportProfile != package.defaultImportProfile)
        throw std::runtime_error(
            "cooked Catalog default import profile mismatch");
    if (catalog.defaultEnvironment)
        throw std::runtime_error(
            "cooked native scene Catalog cannot define a default environment");
    if (catalog.sceneDocuments.size() != package.sceneIds.size())
        throw std::runtime_error(
            "runtime package scene roots do not match the cooked Catalog");

    const SceneDocumentReferences references = catalog.documentReferences();
    std::unordered_set<std::string> modelIds;
    std::unordered_set<std::string> environmentIds;
    for (size_t index = 0; index < catalog.sceneDocuments.size(); ++index) {
        const CatalogSceneDocument &entry = catalog.sceneDocuments[index];
        if (entry.id != package.sceneIds[index])
            throw std::runtime_error(
                "runtime package scene order does not match the cooked Catalog");
        const LoadedSceneDocument loaded = SceneDocumentService::load(
            packageRoot / entry.source, packageRoot, &references);
        if (loaded.document.id.value() != entry.id)
            throw std::runtime_error(
                "packaged SceneDocument ID does not match its Catalog entry");
        for (const SceneEntityDocument &entity : loaded.document.entities) {
            if (entity.modelInstance)
                modelIds.insert(entity.modelInstance->model.value());
        }
        if (loaded.document.environment) {
            environmentIds.insert(
                loaded.document.environment->environmentId);
        }
    }

    for (const CatalogModel &model : catalog.models) {
        if (modelIds.erase(model.id) == 0)
            throw std::runtime_error("cooked Catalog model is not referenced: " +
                                     model.id);
        if (model.type != "gltf")
            throw std::runtime_error(
                "cooked Native Scene contains a non-instanceable model: " +
                model.id);
        const ImportProfile &profile = catalog.profile(model.importProfile);
        if (profile.textureEncoder != "bc7")
            throw std::runtime_error("cooked model is not Native BC7: " +
                                     model.id);
        const std::filesystem::path source = requirePackageFile(
            packageRoot, model.source, "model source");
        verifyGltfBuffers(packageRoot, model);
        DerivedTextureManifest textureManifest;
        std::string error;
        const std::filesystem::path manifestPath = derivedManifestPath(
            packageRoot / package.cacheRoot, model.id, profile.id);
        if (!loadDerivedTextureManifest(manifestPath, textureManifest, error))
            throw std::runtime_error("invalid cooked texture manifest for '" +
                                     model.id + "': " + error);
        if (textureManifest.projectId != catalog.projectId ||
            textureManifest.sceneId != model.id ||
            textureManifest.profileId != profile.id ||
            textureManifest.textureEncoder != TextureEncoder::Bc7)
            throw std::runtime_error(
                "cooked texture manifest identity mismatch: " + model.id);
        for (const DerivedTextureEntry &entry : textureManifest.entries) {
            if (entry.payloadKind != DerivedTexturePayloadKind::NativeBc7)
                throw std::runtime_error(
                    "cooked texture payload is not Native BC7: " + model.id);
            requirePackageFile(packageRoot,
                               std::filesystem::path(package.cacheRoot) /
                                   entry.blob,
                               "texture blob");
        }
        const ArtifactStatus textureStatus = inspectTextureArtifacts(
            {packageRoot / package.cacheRoot, source, catalog.projectId,
             model.id, profile.id, profile.textureLimit,
             TextureEncoder::Bc7});
        if (!textureStatus.ready()) {
            throw std::runtime_error(
                "cooked texture artifacts are invalid for '" + model.id +
                "': " + textureStatus.reason);
        }
        (void)source;
    }
    for (const PrimitiveModelDefinition &primitive :
         primitiveModelDefinitions()) {
        modelIds.erase(std::string(primitive.id));
    }
    if (!modelIds.empty())
        throw std::runtime_error("packaged SceneDocument model is missing");

    if (environmentIds.size() != catalog.environments.size())
        throw std::runtime_error(
            "cooked Catalog contains unreferenced environments");
    for (const CatalogEnvironment &environment : catalog.environments) {
        if (environmentIds.erase(environment.id) == 0) {
            throw std::runtime_error(
                "cooked Catalog environment is not referenced: " +
                environment.id);
        }
        DerivedEnvironmentManifest environmentManifest;
        std::string error;
        const std::filesystem::path manifestPath =
            derivedEnvironmentManifestPath(
                packageRoot / package.cacheRoot, environment.id,
                environment.environmentProfile);
        if (!loadDerivedEnvironmentManifest(
                manifestPath, environmentManifest, error)) {
            throw std::runtime_error(
                "invalid cooked environment manifest for '" +
                environment.id + "': " + error);
        }
        if (environmentManifest.projectId != catalog.projectId ||
            environmentManifest.environmentId != environment.id ||
            environmentManifest.profileId != environment.environmentProfile)
            throw std::runtime_error(
                "cooked environment manifest identity mismatch: " +
                environment.id);
        for (const DerivedEnvironmentImage &image :
             environmentManifest.images) {
            requirePackageFile(packageRoot,
                               std::filesystem::path(package.cacheRoot) /
                                   image.blob,
                               "environment blob");
        }
        const ArtifactStatus environmentStatus =
            inspectEnvironmentArtifacts(
                {packageRoot / package.cacheRoot,
                 packageRoot / environment.source, catalog.projectId,
                 environment.id, environment.environmentProfile});
        if (!environmentStatus.ready()) {
            throw std::runtime_error(
                "cooked environment artifacts are invalid for '" +
                environment.id + "': " + environmentStatus.reason);
        }
    }
    if (!environmentIds.empty())
        throw std::runtime_error(
            "packaged SceneDocument environment is missing");

    verifyArtifactIndexClosure(packageRoot, package, catalog);
    verifyShaderClosure(packageRoot);
}

Json toJson(const RuntimePackageManifest &manifest) {
    Json files = Json::array();
    for (const RuntimePackageFile &file : manifest.files) {
        files.push_back({{"path", file.path},
                         {"bytes", file.bytes},
                         {"sha256", file.sha256}});
    }
    Json root = {{"schemaVersion", manifest.schemaVersion},
                 {"platform", manifest.platform},
                 {"projectId", manifest.projectId},
                 {"requiredTextureEncoder", manifest.requiredTextureEncoder},
                 {"catalog", manifest.catalogPath},
                 {"cacheRoot", manifest.cacheRoot},
                 {"files", std::move(files)}};
    if (manifest.schemaVersion >= RuntimePackageManifest::kSchemaVersion) {
        root["defaultImportProfile"] = manifest.defaultImportProfile;
        root["sceneIds"] = manifest.sceneIds;
        root["startupSceneId"] = manifest.startupSceneId;
        root["runtimeBuild"] = {
            {"revision", manifest.runtimeBuild.revision},
            {"configuration", manifest.runtimeBuild.configuration},
            {"features",
             {{"editorUi", manifest.runtimeBuild.editorUi},
              {"runtimeControl", manifest.runtimeBuild.runtimeControl},
              {"capture", manifest.runtimeBuild.capture},
              {"assetAuthoring", manifest.runtimeBuild.assetAuthoring},
              {"validation", manifest.runtimeBuild.validation},
              {"gpuDebugUtils", manifest.runtimeBuild.gpuDebugUtils},
              {"gpuProfiling", manifest.runtimeBuild.gpuProfiling},
              {"tracy", manifest.runtimeBuild.tracy},
              {"cacao", manifest.runtimeBuild.cacao}}}};
    } else {
        root["profileId"] = manifest.profileId;
    }
    return root;
}

void validateManifest(const RuntimePackageManifest &manifest) {
    if (manifest.schemaVersion != RuntimePackageManifest::kSchemaVersion &&
        manifest.schemaVersion !=
            RuntimePackageManifest::kNativeTextureSchemaVersion &&
        manifest.schemaVersion != RuntimePackageManifest::kLegacySchemaVersion)
        throw std::runtime_error("unsupported runtime package schema");
    if (manifest.platform != "windows-x64")
        throw std::runtime_error("unsupported runtime package platform: " +
                                 manifest.platform);
    const std::string &profileId =
        manifest.schemaVersion >= RuntimePackageManifest::kSchemaVersion
            ? manifest.defaultImportProfile
            : manifest.profileId;
    if (!isStableAssetId(manifest.projectId) || !isStableAssetId(profileId)) {
        throw std::runtime_error(
            "runtime package project/profile ID is invalid");
    }
    if (manifest.schemaVersion >=
            RuntimePackageManifest::kNativeTextureSchemaVersion &&
        manifest.requiredTextureEncoder != "bc7") {
        throw std::runtime_error(
            "runtime package must declare the native BC7 texture requirement");
    }
    if (manifest.schemaVersion >= RuntimePackageManifest::kSchemaVersion) {
        if (manifest.sceneIds.empty() ||
            !isStableAssetId(manifest.startupSceneId)) {
            throw std::runtime_error(
                "native runtime package has no valid startup scene");
        }
        std::set<std::string> sceneIds;
        for (const std::string &sceneId : manifest.sceneIds) {
            if (!isStableAssetId(sceneId) || !sceneIds.insert(sceneId).second)
                throw std::runtime_error(
                    "native runtime package scene IDs are invalid");
        }
        if (sceneIds.count(manifest.startupSceneId) == 0)
            throw std::runtime_error(
                "runtime package startup scene is not packaged");
        const RuntimePackageBuildInfo &build = manifest.runtimeBuild;
        if (build.revision.empty() || build.configuration != "Release")
            throw std::runtime_error(
                "runtime package was not built from a Release runtime");
        if (build.editorUi || build.runtimeControl || build.capture ||
            build.assetAuthoring || build.validation || build.gpuDebugUtils ||
            build.gpuProfiling || build.tracy || build.cacao) {
            throw std::runtime_error(
                "runtime package contains development-only compiled features");
        }
    }
    if (!isSafeRelativePath(manifest.catalogPath) ||
        !isSafeRelativePath(manifest.cacheRoot)) {
        throw std::runtime_error("runtime package metadata path is unsafe");
    }
    std::set<std::string> paths;
    std::string previous;
    for (const RuntimePackageFile &file : manifest.files) {
        if (!isSafeRelativePath(file.path) ||
            file.path == "package_manifest.json" || file.bytes == 0 ||
            !isSha256(file.sha256)) {
            throw std::runtime_error("invalid runtime package file record: " +
                                     file.path);
        }
        if (!previous.empty() && file.path < previous)
            throw std::runtime_error(
                "runtime package file records are not sorted");
        if (!paths.insert(file.path).second)
            throw std::runtime_error("duplicate runtime package file: " +
                                     file.path);
        previous = file.path;
    }
    if (paths.count(manifest.catalogPath) == 0)
        throw std::runtime_error("runtime package catalog is not listed");
    if (paths.count("VulkanLab.exe") == 0)
        throw std::runtime_error("runtime package executable is not listed");
}

} // namespace

bool saveRuntimePackageManifest(const std::filesystem::path &path,
                                const RuntimePackageManifest &manifest,
                                std::string &error) {
    try {
        validateManifest(manifest);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("could not create package manifest");
        output << toJson(manifest).dump(2) << '\n';
        if (!output)
            throw std::runtime_error("could not write package manifest");
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool loadRuntimePackageManifest(const std::filesystem::path &path,
                                RuntimePackageManifest &manifest,
                                std::string &error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("package manifest not found");
        Json root;
        input >> root;
        RuntimePackageManifest loaded;
        loaded.schemaVersion = root.at("schemaVersion").get<uint32_t>();
        loaded.platform = root.at("platform").get<std::string>();
        loaded.projectId = root.at("projectId").get<std::string>();
        if (loaded.schemaVersion >= RuntimePackageManifest::kSchemaVersion) {
            loaded.defaultImportProfile =
                root.at("defaultImportProfile").get<std::string>();
            loaded.profileId = loaded.defaultImportProfile;
            loaded.sceneIds =
                root.at("sceneIds").get<std::vector<std::string>>();
            loaded.startupSceneId =
                root.at("startupSceneId").get<std::string>();
            const Json &runtimeBuild = root.at("runtimeBuild");
            loaded.runtimeBuild.revision =
                runtimeBuild.at("revision").get<std::string>();
            loaded.runtimeBuild.configuration =
                runtimeBuild.at("configuration").get<std::string>();
            const Json &features = runtimeBuild.at("features");
            loaded.runtimeBuild.editorUi = features.at("editorUi").get<bool>();
            loaded.runtimeBuild.runtimeControl =
                features.at("runtimeControl").get<bool>();
            loaded.runtimeBuild.capture = features.at("capture").get<bool>();
            loaded.runtimeBuild.assetAuthoring =
                features.at("assetAuthoring").get<bool>();
            loaded.runtimeBuild.validation =
                features.at("validation").get<bool>();
            loaded.runtimeBuild.gpuDebugUtils =
                features.at("gpuDebugUtils").get<bool>();
            loaded.runtimeBuild.gpuProfiling =
                features.at("gpuProfiling").get<bool>();
            loaded.runtimeBuild.tracy = features.at("tracy").get<bool>();
            loaded.runtimeBuild.cacao = features.value("cacao", false);
        } else {
            loaded.profileId = root.at("profileId").get<std::string>();
            loaded.defaultImportProfile = loaded.profileId;
        }
        loaded.requiredTextureEncoder =
            root.value("requiredTextureEncoder", std::string{});
        loaded.catalogPath = root.at("catalog").get<std::string>();
        loaded.cacheRoot = root.at("cacheRoot").get<std::string>();
        for (const Json &item : root.at("files")) {
            loaded.files.push_back({item.at("path").get<std::string>(),
                                    item.at("bytes").get<uint64_t>(),
                                    item.at("sha256").get<std::string>()});
        }
        validateManifest(loaded);
        manifest = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

RuntimePackageVerification verifyRuntimePackage(
    const std::filesystem::path &packageRoot,
    const RuntimePackageManifest &manifest) {
    validateManifest(manifest);
    const std::filesystem::path root =
        std::filesystem::absolute(packageRoot).lexically_normal();
    RuntimePackageVerification result;
    for (const RuntimePackageFile &file : manifest.files) {
        const std::filesystem::path path =
            (root / std::filesystem::path(file.path)).lexically_normal();
        if (!pathIsWithin(root, path) ||
            !std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("runtime package file is missing: " +
                                     file.path);
        }
        std::error_code sizeError;
        const uint64_t bytes = std::filesystem::file_size(path, sizeError);
        if (sizeError || bytes != file.bytes)
            throw std::runtime_error(
                "runtime package file size mismatch: " + file.path);
        if (sha256File(path) != file.sha256)
            throw std::runtime_error(
                "runtime package file hash mismatch: " + file.path);
        ++result.fileCount;
        result.totalBytes += bytes;
    }
    if (manifest.schemaVersion >= RuntimePackageManifest::kSchemaVersion)
        verifyNativeSceneClosure(root, manifest);
    return result;
}

std::optional<std::filesystem::path> findRuntimePackageRoot(
    const std::filesystem::path &executablePath) {
    if (executablePath.empty())
        return std::nullopt;
    const std::filesystem::path root = executablePath.parent_path();
    if (std::filesystem::is_regular_file(root / "package_manifest.json"))
        return root;
    return std::nullopt;
}

} // namespace vkr
