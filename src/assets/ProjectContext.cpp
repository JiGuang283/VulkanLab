#include "ProjectContext.h"
#include "RuntimePackage.h"

#include <json.hpp>

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vkr {

namespace {

constexpr const char *kCatalogRelativePath = "assets/catalog.json";
constexpr const char *kLocatorName = "vulkanlab_project.json";

std::filesystem::path normalizeExistingDirectory(
    const std::filesystem::path &path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error)
        throw std::runtime_error("Could not resolve project path: " +
                                 path.string());
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(absolute, error);
    if (error || !std::filesystem::is_directory(canonical))
        throw std::runtime_error("Project root is not a directory: " +
                                 absolute.string());
    return canonical;
}

std::optional<std::filesystem::path>
readLocator(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    try {
        nlohmann::json root;
        input >> root;
        const std::string projectRoot =
            root.at("projectRoot").get<std::string>();
        if (projectRoot.empty())
            throw std::runtime_error("projectRoot is empty");
        return std::filesystem::path(projectRoot);
    } catch (const std::exception &exception) {
        throw std::runtime_error("Invalid project locator '" + path.string() +
                                 "': " + exception.what());
    }
}

std::optional<std::filesystem::path>
findProjectFromAncestors(std::filesystem::path start) {
    std::error_code error;
    start = std::filesystem::absolute(start, error);
    if (error)
        return std::nullopt;
    while (!start.empty()) {
        if (std::filesystem::is_regular_file(start / kCatalogRelativePath,
                                             error))
            return start;
        const std::filesystem::path parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return std::nullopt;
}

std::filesystem::path resolveFromRoot(const std::filesystem::path &root,
                                      const std::filesystem::path &path) {
    if (path.is_absolute())
        return path.lexically_normal();
    return (root / path).lexically_normal();
}

std::filesystem::path normalizeOutputDirectory(
    const std::filesystem::path &path) {
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(path, error);
    if (error)
        throw std::runtime_error("Could not resolve workspace path: " +
                                 path.string());
    return absolute.lexically_normal();
}

std::string safePathComponent(std::string value) {
    if (value.empty())
        return "default";
    for (char &character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '-' && character != '_')
            character = '_';
    }
    return value;
}

std::string projectIdFromCatalog(const std::filesystem::path &catalogPath) {
    std::ifstream input(catalogPath, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read project catalog: " +
                                 catalogPath.string());
    try {
        nlohmann::json root;
        input >> root;
        const std::string projectId = root.at("projectId").get<std::string>();
        if (projectId.empty())
            throw std::runtime_error("projectId is empty");
        return projectId;
    } catch (const std::exception &exception) {
        throw std::runtime_error("Could not determine project ID from '" +
                                 catalogPath.string() + "': " +
                                 exception.what());
    }
}

void configureWorkspace(
    ProjectContext &context, const std::string &projectId,
    const std::optional<std::filesystem::path> &explicitWorkspaceRoot) {
    context.workspaceRoot = normalizeOutputDirectory(
        explicitWorkspaceRoot
            ? *explicitWorkspaceRoot
            : ProjectContextResolver::defaultWorkspaceRoot(projectId));
    context.logRoot = context.workspaceRoot / "logs";
    context.captureRoot = context.workspaceRoot / "captures";
    context.diagnosticsRoot = context.workspaceRoot / "diagnostics";
    context.tempRoot = context.workspaceRoot / "temp";
}

ProjectContext makeContext(const std::filesystem::path &root,
                           const std::filesystem::path &runtimeRoot,
                           std::string diagnostic) {
    ProjectContext context;
    context.projectRoot = normalizeExistingDirectory(root);
    context.runtimeRoot = normalizeExistingDirectory(runtimeRoot);
    context.catalogPath = context.projectRoot / kCatalogRelativePath;
    if (!std::filesystem::is_regular_file(context.catalogPath))
        throw std::runtime_error("Project catalog not found: " +
                                 context.catalogPath.string());
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(context.catalogPath.c_str());
    context.catalogWritable =
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) == 0;
#else
    const auto permissions = std::filesystem::status(context.catalogPath).permissions();
    context.catalogWritable =
        (permissions & std::filesystem::perms::owner_write) !=
        std::filesystem::perms::none;
#endif
    context.diagnostic = std::move(diagnostic);
    return context;
}

ProjectContext makePackageContext(
    const std::filesystem::path &root,
    const std::optional<std::filesystem::path> &explicitWorkspaceRoot) {
    RuntimePackageManifest manifest;
    std::string error;
    const std::filesystem::path manifestPath = root / "package_manifest.json";
    if (!loadRuntimePackageManifest(manifestPath, manifest, error)) {
        throw std::runtime_error("Invalid runtime package '" +
                                 manifestPath.string() + "': " + error);
    }
    const RuntimePackageVerification verified =
        verifyRuntimePackage(root, manifest);
    ProjectContext context;
    context.projectRoot = normalizeExistingDirectory(root);
    context.runtimeRoot = context.projectRoot;
    context.catalogPath = context.projectRoot / manifest.catalogPath;
    context.cacheRoot = context.projectRoot / manifest.cacheRoot;
    context.catalogWritable = false;
    context.cookedPackage = true;
    context.packageSchemaVersion = manifest.schemaVersion;
    context.nativeScenePackage =
        manifest.schemaVersion >= RuntimePackageManifest::kSchemaVersion;
    context.packageProfileId = manifest.defaultImportProfile.empty()
                                   ? manifest.profileId
                                   : manifest.defaultImportProfile;
    context.startupSceneId = manifest.startupSceneId;
    context.requiredTextureEncoder = manifest.requiredTextureEncoder;
    context.diagnostic = "verified cooked package (" +
                         std::to_string(verified.fileCount) + " files)";
    configureWorkspace(context, manifest.projectId, explicitWorkspaceRoot);
    return context;
}

} // namespace

ProjectContext ProjectContextResolver::resolve(
    const std::optional<std::filesystem::path> &explicitProjectRoot,
    const std::filesystem::path &executablePath,
    const std::optional<std::filesystem::path> &explicitWorkspaceRoot) {
    std::vector<std::filesystem::path> locatorCandidates;
    const std::filesystem::path executable =
        executablePath.empty() ? currentExecutablePath() : executablePath;
    if (const auto packageRoot = findRuntimePackageRoot(executable)) {
        if (explicitProjectRoot) {
            throw std::runtime_error(
                "--project cannot override a cooked runtime package");
        }
        return makePackageContext(*packageRoot, explicitWorkspaceRoot);
    }
    if (executable.empty() || executable.parent_path().empty())
        throw std::runtime_error("Could not determine the runtime directory");
    const std::filesystem::path runtimeRoot = executable.parent_path();
    if (explicitProjectRoot) {
        ProjectContext context = makeContext(*explicitProjectRoot, runtimeRoot,
                                             "explicit --project");
        configureWorkspace(context, projectIdFromCatalog(context.catalogPath),
                           explicitWorkspaceRoot);
        return context;
    }

    if (!executable.empty())
        locatorCandidates.push_back(executable.parent_path() / kLocatorName);
    locatorCandidates.push_back(std::filesystem::current_path() / kLocatorName);

    for (const auto &candidate : locatorCandidates) {
        if (const auto root = readLocator(candidate)) {
            ProjectContext context =
                makeContext(*root, runtimeRoot,
                            "developer locator " + candidate.string());
            configureWorkspace(context,
                               projectIdFromCatalog(context.catalogPath),
                               explicitWorkspaceRoot);
            return context;
        }
    }

    if (const auto root =
            findProjectFromAncestors(std::filesystem::current_path())) {
        ProjectContext context = makeContext(
            *root, runtimeRoot,
            "catalog found in working-directory ancestor");
        configureWorkspace(context, projectIdFromCatalog(context.catalogPath),
                           explicitWorkspaceRoot);
        return context;
    }

    throw std::runtime_error(
        "Could not locate a VulkanLab project. Pass --project <path> or run "
        "beside a generated vulkanlab_project.json locator.");
}

std::filesystem::path
ProjectContextResolver::defaultWorkspaceRoot(const std::string &projectId) {
    const std::string component = safePathComponent(projectId);
#ifdef _WIN32
    const DWORD required =
        GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1) {
        std::wstring localAppData(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"LOCALAPPDATA", localAppData.data(), required);
        if (written > 0 && written < required) {
            localAppData.resize(written);
            return std::filesystem::path(localAppData) / "VulkanLab" /
                   "Workspaces" / component;
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "VulkanLab" /
           "Workspaces" / component;
}

std::filesystem::path ProjectContext::resolveProjectPath(
    const std::filesystem::path &path) const {
    return resolveFromRoot(projectRoot, path);
}

std::filesystem::path ProjectContext::resolveRuntimePath(
    const std::filesystem::path &path) const {
    return resolveFromRoot(runtimeRoot, path);
}

std::filesystem::path ProjectContextResolver::currentExecutablePath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

} // namespace vkr
