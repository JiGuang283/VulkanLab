#include "SceneImportService.h"

#include "DerivedTextureManifest.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vkr {

namespace {

using Json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546c67u;
constexpr uint32_t kGlbJsonChunk = 0x4e4f534au;

uint32_t readU32(const std::vector<uint8_t> &bytes, size_t offset) {
    if (offset + 4 > bytes.size())
        throw std::runtime_error("GLB header is truncated");
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

Json readSceneJson(const std::filesystem::path &path) {
    const std::string extension = [&] {
        std::string value = path.extension().string();
        std::transform(value.begin(), value.end(), value.begin(), [](char c) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        });
        return value;
    }();
    if (extension == ".gltf") {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Could not read glTF file: " +
                                     path.string());
        Json root;
        input >> root;
        return root;
    }
    if (extension != ".glb")
        throw std::runtime_error("Scene import only supports .gltf and .glb");

    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> header(20);
    if (!input.read(reinterpret_cast<char *>(header.data()),
                    static_cast<std::streamsize>(header.size())))
        throw std::runtime_error("GLB header is truncated: " + path.string());
    const uint64_t fileSize = std::filesystem::file_size(path);
    if (readU32(header, 0) != kGlbMagic || readU32(header, 4) != 2u ||
        readU32(header, 8) != fileSize)
        throw std::runtime_error("Invalid GLB 2.0 header: " + path.string());
    const uint32_t chunkLength = readU32(header, 12);
    const uint32_t chunkType = readU32(header, 16);
    if (chunkType != kGlbJsonChunk || 20ull + chunkLength > fileSize)
        throw std::runtime_error("GLB JSON chunk is missing or truncated");
    std::string json(chunkLength, '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(chunkLength)))
        throw std::runtime_error("GLB JSON chunk is truncated");
    while (!json.empty() &&
           (json.back() == '\0' || std::isspace(
                                     static_cast<unsigned char>(json.back()))))
        json.pop_back();
    return Json::parse(json);
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

std::string decodeUriPath(const std::string &uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] != '%') {
            decoded.push_back(uri[i]);
            continue;
        }
        if (i + 2 >= uri.size())
            throw std::runtime_error("Invalid percent escape in URI: " + uri);
        const int high = hexDigit(uri[i + 1]);
        const int low = hexDigit(uri[i + 2]);
        if (high < 0 || low < 0)
            throw std::runtime_error("Invalid percent escape in URI: " + uri);
        const char value = static_cast<char>((high << 4) | low);
        if (value == '\0')
            throw std::runtime_error("NUL is not allowed in a URI path");
        decoded.push_back(value);
        i += 2;
    }
    return decoded;
}

bool isDataUri(const std::string &uri) { return uri.rfind("data:", 0) == 0; }

bool hasUriScheme(const std::string &uri) {
    const size_t colon = uri.find(':');
    const size_t slash = uri.find_first_of("/\\");
    return colon != std::string::npos &&
           (slash == std::string::npos || colon < slash);
}

std::filesystem::path normalizedExistingFile(
    const std::filesystem::path &path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error || !std::filesystem::is_regular_file(absolute))
        throw std::runtime_error("Scene source is missing: " + path.string());
    const auto canonical = std::filesystem::canonical(absolute, error);
    if (error)
        throw std::runtime_error("Could not resolve scene source: " +
                                 path.string());
    return canonical;
}

void appendDependency(const std::string &uri,
                      const std::filesystem::path &sourceRoot,
                      std::vector<SceneImportDependency> &dependencies,
                      std::set<std::filesystem::path> &seen) {
    if (uri.empty() || isDataUri(uri))
        return;
    if (hasUriScheme(uri))
        throw std::runtime_error("Remote or unsupported URI is not allowed: " +
                                 uri);
    if (uri.find('\\') != std::string::npos)
        throw std::runtime_error("Backslashes are not allowed in glTF URI: " +
                                 uri);
    const std::string decoded = decodeUriPath(uri);
    const std::filesystem::path relative =
        std::filesystem::path(decoded).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("Dependency URI is not relative: " + uri);
    const std::filesystem::path candidate = sourceRoot / relative;
    if (!pathIsWithin(sourceRoot, candidate))
        throw std::runtime_error("Dependency URI escapes the source root: " +
                                 uri);
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::canonical(candidate, error);
    if (error || !std::filesystem::is_regular_file(canonical))
        throw std::runtime_error("Dependency is missing: " + uri);
    if (!pathIsWithin(sourceRoot, canonical))
        throw std::runtime_error("Dependency symlink escapes source root: " +
                                 uri);
    if (seen.insert(relative).second)
        dependencies.push_back({uri, canonical, relative});
}

void collectUris(const Json &root, const char *arrayName,
                 const std::filesystem::path &sourceRoot,
                 std::vector<SceneImportDependency> &dependencies,
                 std::set<std::filesystem::path> &seen) {
    if (!root.contains(arrayName))
        return;
    const Json &items = root.at(arrayName);
    if (!items.is_array())
        throw std::runtime_error(std::string("glTF '") + arrayName +
                                 "' must be an array");
    for (const Json &item : items) {
        if (item.contains("uri") && item.at("uri").is_string())
            appendDependency(item.at("uri").get<std::string>(), sourceRoot,
                             dependencies, seen);
    }
}

std::string displayNameFromStem(const std::filesystem::path &path) {
    std::string value = path.stem().string();
    for (char &character : value) {
        if (character == '_' || character == '-')
            character = ' ';
    }
    return value.empty() ? std::string("Imported Scene") : value;
}

void checkCancelled(const SceneImportCancel &cancel) {
    if (cancel && cancel())
        throw std::runtime_error("Scene import cancelled");
}

std::string uniqueSuffix() {
    return std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count());
}

void copyFileWithProgress(const std::filesystem::path &source,
                          const std::filesystem::path &destination,
                          uint64_t &completedBytes, uint64_t totalBytes,
                          const SceneImportCancel &cancel,
                          const SceneImportProgressCallback &progress) {
    checkCancelled(cancel);
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::none);
    completedBytes += std::filesystem::file_size(source);
    if (progress)
        progress({completedBytes, totalBytes, source.filename().string()});
}

bool sameStamp(const DerivedFileStamp &left, const DerivedFileStamp &right) {
    return left.size == right.size && left.writeTime == right.writeTime;
}

void atomicReplace(const std::filesystem::path &temporary,
                   const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not atomically replace Catalog (error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    std::filesystem::rename(temporary, destination);
#endif
}

void appendCatalogScene(const ProjectContext &project,
                        const SceneImportRequest &request,
                        const std::filesystem::path &projectRelativeSource) {
    const DerivedFileStamp before = fileStamp(project.catalogPath);
    std::ifstream input(project.catalogPath, std::ios::binary);
    Json root;
    input >> root;
    input.close();

    for (const Json &item : root.at("scenes")) {
        if (item.value("id", std::string{}) == request.sceneId)
            throw std::runtime_error("Scene ID already exists: " +
                                     request.sceneId);
        std::string existing = item.value("displayName", std::string{});
        std::string requested = request.displayName;
        std::transform(existing.begin(), existing.end(), existing.begin(),
                       [](char c) { return static_cast<char>(std::tolower(
                                              static_cast<unsigned char>(c))); });
        std::transform(requested.begin(), requested.end(), requested.begin(),
                       [](char c) { return static_cast<char>(std::tolower(
                                              static_cast<unsigned char>(c))); });
        if (existing == requested)
            throw std::runtime_error("Scene display name already exists: " +
                                     request.displayName);
    }

    root["scenes"].push_back({{"id", request.sceneId},
                              {"displayName", request.displayName},
                              {"source", projectRelativeSource.generic_string()},
                              {"importProfile", request.profileId}});

    const std::filesystem::path temporary =
        project.catalogPath.string() + ".import-" + uniqueSuffix() + ".tmp";
    try {
        {
            std::ofstream output(temporary,
                                 std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("Could not create temporary Catalog");
            output << root.dump(2) << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error("Could not flush temporary Catalog");
        }
        (void)SceneCatalog::load(temporary, project.projectRoot);
        if (!sameStamp(before, fileStamp(project.catalogPath)))
            throw std::runtime_error(
                "Catalog changed during import; refusing to overwrite it");
        atomicReplace(temporary, project.catalogPath);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace

SceneImportPreflight
SceneImportService::preflight(const std::filesystem::path &sourcePath) {
    SceneImportPreflight result;
    result.sourcePath = normalizedExistingFile(sourcePath);
    result.suggestedDisplayName = displayNameFromStem(result.sourcePath);
    result.suggestedSceneId = suggestSceneId(result.suggestedDisplayName);

    const Json root = readSceneJson(result.sourcePath);
    if (!root.is_object() || !root.contains("asset"))
        throw std::runtime_error("Scene does not contain glTF asset metadata");
    const std::filesystem::path sourceRoot = result.sourcePath.parent_path();
    std::set<std::filesystem::path> seen;
    collectUris(root, "buffers", sourceRoot, result.dependencies, seen);
    collectUris(root, "images", sourceRoot, result.dependencies, seen);
    const auto collectExtensions = [&](const char *name,
                                       std::vector<std::string> &output) {
        if (!root.contains(name))
            return;
        if (!root.at(name).is_array())
            throw std::runtime_error(std::string(name) +
                                     " must be an array");
        for (const Json &extension : root.at(name)) {
            if (!extension.is_string())
                throw std::runtime_error(std::string(name) +
                                         " must contain strings");
            output.push_back(extension.get<std::string>());
        }
    };
    collectExtensions("extensionsUsed", result.extensionsUsed);
    collectExtensions("extensionsRequired", result.extensionsRequired);

    result.totalBytes = std::filesystem::file_size(result.sourcePath);
    for (const auto &dependency : result.dependencies)
        result.totalBytes += std::filesystem::file_size(dependency.sourcePath);
    return result;
}

SceneImportResult SceneImportService::importScene(
    const ProjectContext &project, const SceneImportRequest &request,
    const SceneImportCancel &cancel,
    const SceneImportProgressCallback &progress) {
    if (!project.catalogWritable)
        throw std::runtime_error("The project Catalog is read-only");
    if (!isStableAssetId(request.sceneId))
        throw std::runtime_error("Scene ID is not a stable lowercase ID");
    if (request.displayName.empty())
        throw std::runtime_error("Scene display name cannot be empty");
    const SceneCatalog catalog =
        SceneCatalog::load(project.catalogPath, project.projectRoot);
    (void)catalog.profile(request.profileId);
    if (catalog.findScene(request.sceneId))
        throw std::runtime_error("Scene ID already exists: " + request.sceneId);

    const SceneImportPreflight checked = preflight(request.sourcePath);
    if (!request.validation)
        throw std::runtime_error(
            "Scene import requires a validation receipt");
    const AssetValidationQuery validation = queryValidationReceipt(
        project.cacheRoot, *request.validation, checked.sourcePath);
    const bool validated =
        validation.state == AssetValidationState::Valid ||
        validation.state == AssetValidationState::Warnings;
    const bool explicitlyUnvalidated =
        validation.state == AssetValidationState::Unavailable &&
        request.allowUnvalidated;
    if (!validated && !explicitlyUnvalidated) {
        throw std::runtime_error(
            "Scene validation gate rejected import: " +
            std::string(assetValidationStateName(validation.state)) +
            (validation.reason.empty() ? std::string{}
                                       : " (" + validation.reason + ")"));
    }
    checkCancelled(cancel);

    std::filesystem::path projectRelativeSource;
    std::filesystem::path publishedDirectory;
    std::filesystem::path stagingDirectory;
    bool validationBound = false;
    try {
        if (request.placement == SceneImportPlacement::ReferenceExisting) {
            if (!pathIsWithin(project.projectRoot, checked.sourcePath))
                throw std::runtime_error(
                    "Reference Existing requires a project-local source");
            projectRelativeSource = std::filesystem::relative(
                checked.sourcePath, project.projectRoot);
        } else {
            const std::filesystem::path importedRoot =
                project.projectRoot / "models/imported";
            publishedDirectory = importedRoot / request.sceneId;
            if (std::filesystem::exists(publishedDirectory))
                throw std::runtime_error("Import destination already exists: " +
                                         publishedDirectory.string());
            stagingDirectory =
                importedRoot / (".staging-" + request.sceneId + "-" +
                                uniqueSuffix());
            std::filesystem::create_directories(stagingDirectory);

            uint64_t completed = 0;
            const std::filesystem::path stagedScene =
                stagingDirectory / checked.sourcePath.filename();
            copyFileWithProgress(checked.sourcePath, stagedScene, completed,
                                 checked.totalBytes, cancel, progress);
            for (const auto &dependency : checked.dependencies) {
                copyFileWithProgress(
                    dependency.sourcePath,
                    stagingDirectory / dependency.relativePath, completed,
                    checked.totalBytes, cancel, progress);
            }
            checkCancelled(cancel);
            (void)preflight(stagedScene);
            const AssetValidationQuery afterCopy = queryValidationReceipt(
                project.cacheRoot, *request.validation, checked.sourcePath);
            const bool sourceStillValidated =
                afterCopy.state == AssetValidationState::Valid ||
                afterCopy.state == AssetValidationState::Warnings ||
                (afterCopy.state == AssetValidationState::Unavailable &&
                 request.allowUnvalidated);
            if (!sourceStillValidated) {
                throw std::runtime_error(
                    "Scene source or dependencies changed during import: " +
                    std::string(assetValidationStateName(afterCopy.state)) +
                    (afterCopy.reason.empty()
                         ? std::string{}
                         : " (" + afterCopy.reason + ")"));
            }
            std::filesystem::rename(stagingDirectory, publishedDirectory);
            stagingDirectory.clear();
            projectRelativeSource = std::filesystem::relative(
                publishedDirectory / checked.sourcePath.filename(),
                project.projectRoot);
        }

        checkCancelled(cancel);
        (void)bindSceneValidation(project.cacheRoot, project.projectRoot,
                                  request.sceneId, projectRelativeSource,
                                  *request.validation);
        validationBound = true;
        checkCancelled(cancel);
        appendCatalogScene(project, request, projectRelativeSource);
    } catch (...) {
        if (validationBound) {
            try {
                removeSceneValidationBinding(project.cacheRoot,
                                             request.sceneId);
            } catch (...) {
            }
        }
        std::error_code ignored;
        if (!stagingDirectory.empty())
            std::filesystem::remove_all(stagingDirectory, ignored);
        if (!publishedDirectory.empty())
            std::filesystem::remove_all(publishedDirectory, ignored);
        throw;
    }

    const SceneCatalog updated =
        SceneCatalog::load(project.catalogPath, project.projectRoot);
    const CatalogScene *scene = updated.findScene(request.sceneId);
    if (!scene)
        throw std::runtime_error("Imported scene was not published to Catalog");
    return {*scene, project.projectRoot / scene->source};
}

std::string SceneImportService::suggestSceneId(const std::string &name) {
    std::string result;
    bool separator = false;
    for (const unsigned char byte : name) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
            if (separator && !result.empty())
                result.push_back('-');
            result.push_back(static_cast<char>(byte));
            separator = false;
        } else if (byte >= 'A' && byte <= 'Z') {
            if (separator && !result.empty())
                result.push_back('-');
            result.push_back(static_cast<char>(byte - 'A' + 'a'));
            separator = false;
        } else {
            separator = true;
        }
    }
    while (!result.empty() && result.back() == '-')
        result.pop_back();
    return result.empty() ? std::string("scene") : result;
}

} // namespace vkr
