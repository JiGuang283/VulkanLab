#include "RuntimePackage.h"

#include "ContentHash.h"
#include "SceneCatalog.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

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

Json toJson(const RuntimePackageManifest &manifest) {
    Json files = Json::array();
    for (const RuntimePackageFile &file : manifest.files) {
        files.push_back({{"path", file.path},
                         {"bytes", file.bytes},
                         {"sha256", file.sha256}});
    }
    return {{"schemaVersion", manifest.schemaVersion},
            {"platform", manifest.platform},
            {"projectId", manifest.projectId},
            {"profileId", manifest.profileId},
            {"catalog", manifest.catalogPath},
            {"cacheRoot", manifest.cacheRoot},
            {"files", std::move(files)}};
}

void validateManifest(const RuntimePackageManifest &manifest) {
    if (manifest.schemaVersion != RuntimePackageManifest::kSchemaVersion)
        throw std::runtime_error("unsupported runtime package schema");
    if (manifest.platform != "windows-x64")
        throw std::runtime_error("unsupported runtime package platform: " +
                                 manifest.platform);
    if (!isStableAssetId(manifest.projectId) ||
        !isStableAssetId(manifest.profileId)) {
        throw std::runtime_error(
            "runtime package project/profile ID is invalid");
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
        loaded.profileId = root.at("profileId").get<std::string>();
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
