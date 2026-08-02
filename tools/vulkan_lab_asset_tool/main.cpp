#include "TextureCacheBuilder.h"
#include "CookClosureResolver.h"
#include "CookPackageBuilder.h"
#include "EnvironmentCacheBuilder.h"
#include "GltfValidator.h"
#include "ProcessRunner.h"

#include "assets/DerivedAssetPaths.h"
#include "assets/CacheMutationLock.h"
#include "assets/ArtifactCachePruner.h"
#include "assets/ArtifactIndex.h"
#include "assets/ProjectContext.h"
#include "assets/RuntimePackage.h"
#include "assets/SceneCatalog.h"
#include "assets/SceneCatalogEditor.h"
#include "assets/SceneImportService.h"

#include <json.hpp>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {

std::atomic<std::atomic_bool *> gCancelRequested{nullptr};

BOOL WINAPI handleConsoleSignal(DWORD signal) {
    std::atomic_bool *cancelRequested = gCancelRequested.load();
    if ((signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
         signal == CTRL_CLOSE_EVENT) &&
        cancelRequested) {
        cancelRequested->store(true);
        return TRUE;
    }
    return FALSE;
}

class ConsoleCancellationHandler {
  public:
    explicit ConsoleCancellationHandler(std::atomic_bool &cancelRequested) {
        gCancelRequested.store(&cancelRequested);
        installed_ = SetConsoleCtrlHandler(handleConsoleSignal, TRUE) != FALSE;
    }
    ~ConsoleCancellationHandler() {
        if (installed_)
            SetConsoleCtrlHandler(handleConsoleSignal, FALSE);
        gCancelRequested.store(nullptr);
    }

  private:
    bool installed_ = false;
};

void printUsage(std::ostream &output) {
    output
        << "Usage:\n"
        << "  VulkanLabAssetTool import model --model-id <id> "
           "[--profile <id>] [options]\n"
        << "  VulkanLabAssetTool texture-cache build --model-id <id> "
           "[--profile <id>] [options]\n"
        << "  VulkanLabAssetTool texture-cache migrate "
           "--legacy-cache-root <path> [options]\n\n"
        << "  VulkanLabAssetTool catalog add --source <path> [options]\n\n"
        << "  VulkanLabAssetTool validate scene "
           "(--source <path> | --model-id <id>) [options]\n\n"
        << "  VulkanLabAssetTool catalog add-environment --source <file.hdr> "
           "[options]\n"
        << "  VulkanLabAssetTool environment-cache build "
           "--environment-id <id> [--profile <id>] [options]\n\n"
        << "  VulkanLabAssetTool cache index rebuild [options]\n"
        << "  VulkanLabAssetTool cache prune [--older-than-days <n>] "
           "[--execute] [options]\n\n"
        << "  VulkanLabAssetTool cook --platform windows-x64 "
           "--output <path> [--scene-id <native-scene-id>]... "
           "[--startup-scene <id>] [options]\n\n"
        << "  VulkanLabAssetTool package verify --path <package-root>\n\n"
        << "Options:\n"
        << "  --project <path>     Source project root (otherwise use "
           "locator)\n"
        << "  --scene <path>       Legacy alias; match a Catalog source path\n"
        << "  --model-id <id>      Stable Catalog model ID\n"
        << "  --scene-id <id>      Native SceneDocument ID for cook; model "
           "ID elsewhere\n"
        << "  --startup-scene <id> Startup SceneDocument in a cooked package\n"
        << "  --environment-id <id> Stable Catalog environment ID\n"
        << "  --profile <id>       Import profile (default: model profile)\n"
        << "  --texture-limit <n>  Override profile limit: 0/512/1024/2048\n"
        << "  --cache-root <path>  Override the shared derived cache root\n"
        << "  --older-than-days <n>  Retain recent orphan blobs (default: 7)\n"
        << "  --execute            Apply cache prune (default is dry-run)\n"
        << "  --runtime-dir <path> Directory containing VulkanLab.exe and shader\n"
        << "  --output <path>      Cooked package output directory\n"
        << "  --path <path>        Cooked package root to verify\n"
        << "  --platform <name>    Cook target (windows-x64 only)\n"
        << "  --build-missing      Build non-Ready artifacts before cooking\n"
        << "  --legacy-cache-root  Legacy working-directory cache to migrate\n"
        << "  --force              Re-encode blobs even when they are valid\n"
        << "  --workers <n>        Parallel encoder processes (default: CPU/2, "
           "max 4)\n"
        << "  --memory-budget-mib <n>  Encoder working-set budget (default: 2048)\n"
        << "  --preset <name>      development or production\n"
        << "  --progress ndjson    Emit machine-readable progress on stdout\n"
        << "  --progress-json      Alias for --progress ndjson\n"
        << "  --ktx-tool <path>    Path to the KTX 4.4.2 `ktx` executable\n"
        << "  --gltf-validator <path>  Path to glTF Validator "
           "2.0.0-dev.3.10\n"
        << "  --report <path>      Copy a validation report to this path\n"
        << "  --allow-unvalidated  Allow catalog add only when Validator is "
           "unavailable\n"
        << "  --help                Show this help\n";
}

std::string requireValue(int &index, int argc, char **argv,
                         const std::string &option) {
    if (++index >= argc)
        throw std::invalid_argument(option + " requires a value");
    return argv[index];
}

uint32_t parseTextureLimit(const std::string &value) {
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size())
        throw std::invalid_argument("invalid texture limit: " + value);
    static const std::unordered_set<unsigned long> allowed{0, 512, 1024, 2048};
    if (allowed.count(parsed) == 0)
        throw std::invalid_argument(
            "texture limit must be one of 0, 512, 1024, or 2048");
    return static_cast<uint32_t>(parsed);
}

uint32_t parsePositiveUint32(const std::string &value,
                             const std::string &option) {
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > UINT32_MAX)
        throw std::invalid_argument("invalid value for " + option + ": " +
                                    value);
    return static_cast<uint32_t>(parsed);
}

uint32_t parseNonnegativeUint32(const std::string &value,
                                const std::string &option) {
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > UINT32_MAX)
        throw std::invalid_argument("invalid value for " + option + ": " +
                                    value);
    return static_cast<uint32_t>(parsed);
}

std::string stableIdFromStem(std::string value) {
    std::string result;
    bool pendingSeparator = false;
    for (unsigned char byte : value) {
        if (std::isalnum(byte)) {
            if (pendingSeparator && !result.empty())
                result.push_back('-');
            result.push_back(
                static_cast<char>(std::tolower(byte)));
            pendingSeparator = false;
        } else {
            pendingSeparator = true;
        }
    }
    if (result.empty() || !std::isalpha(
                              static_cast<unsigned char>(result.front()))) {
        result = "environment-" + result;
    }
    return result;
}

nlohmann::json validationSummaryJson(
    const vkr::assettool::GltfValidationResult &result) {
    const vkr::AssetValidationReport &report = result.report;
    nlohmann::json extensions = nlohmann::json::array();
    for (const auto &extension : report.extensions) {
        extensions.push_back(
            {{"name", extension.name},
             {"support", vkr::gltfExtensionSupportName(extension.support)},
             {"required", extension.required},
             {"note", extension.note}});
    }
    return {{"state", vkr::assetValidationStateName(report.state)},
            {"validatorVersion", report.validatorVersion},
            {"reportKey", report.reportKey},
            {"inputFingerprint", report.inputFingerprint},
            {"reportPath", result.reportPath.generic_string()},
            {"reused", result.reused},
            {"errors", report.errorCount},
            {"warnings", report.warningCount},
            {"infos", report.infoCount},
            {"hints", report.hintCount},
            {"truncated", report.truncated},
            {"failureReason", report.failureReason},
            {"extensions", std::move(extensions)}};
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            printUsage(std::cout);
            return EXIT_SUCCESS;
        }
        if (argc < 3) {
            printUsage(std::cerr);
            return 2;
        }

        if (std::string(argv[1]) == "validate" &&
            std::string(argv[2]) == "scene") {
            std::optional<std::filesystem::path> explicitProject;
            std::filesystem::path source;
            std::filesystem::path cacheRoot;
            std::filesystem::path validatorPath;
            std::filesystem::path requestedReport;
            std::string sceneId;
            bool force = false;
            bool progressNdjson = false;
            for (int i = 3; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--source") {
                    source = requireValue(i, argc, argv, argument);
                } else if (argument == "--model-id" ||
                           argument == "--scene-id") {
                    sceneId = requireValue(i, argc, argv, argument);
                } else if (argument == "--cache-root") {
                    cacheRoot = requireValue(i, argc, argv, argument);
                } else if (argument == "--gltf-validator") {
                    validatorPath = requireValue(i, argc, argv, argument);
                } else if (argument == "--report") {
                    requestedReport = requireValue(i, argc, argv, argument);
                } else if (argument == "--force") {
                    force = true;
                } else if (argument == "--progress-json") {
                    progressNdjson = true;
                } else if (argument == "--progress") {
                    const std::string mode =
                        requireValue(i, argc, argv, argument);
                    if (mode != "ndjson")
                        throw std::invalid_argument(
                            "--progress currently accepts only ndjson");
                    progressNdjson = true;
                } else if (argument == "--help") {
                    printUsage(std::cout);
                    return EXIT_SUCCESS;
                } else {
                    throw std::invalid_argument("unknown option: " +
                                                argument);
                }
            }
            if (source.empty() == sceneId.empty())
                throw std::invalid_argument(
                    "exactly one of --source or --model-id is required");
            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            if (!sceneId.empty()) {
                const vkr::CatalogModel *model = catalog.findModel(sceneId);
                if (!model)
                    throw std::invalid_argument(
                        "unknown Catalog model: " + sceneId);
                if (model->type != "gltf")
                    throw std::invalid_argument(
                        "model is not a glTF asset: " + sceneId);
                source = project.projectRoot / model->source;
            }
            if (cacheRoot.empty())
                cacheRoot = vkr::DerivedAssetPaths::defaultCacheRoot(
                    catalog.projectId);
            project.cacheRoot =
                std::filesystem::absolute(cacheRoot).lexically_normal();

            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            vkr::assettool::Win32JobProcessRunner processRunner;
            vkr::assettool::GltfValidationOptions options;
            options.sourcePath = source;
            options.cacheRoot = cacheRoot;
            options.validatorPath = validatorPath;
            options.force = force;
            const auto result = vkr::assettool::validateGltf(
                options, cancelRequested, processRunner);
            if (!sceneId.empty()) {
                (void)vkr::bindSceneValidation(
                    cacheRoot, project.projectRoot, sceneId,
                    catalog.findModel(sceneId)->source,
                    vkr::sceneValidationReceipt(result.report));
            }
            if (!requestedReport.empty()) {
                requestedReport = std::filesystem::absolute(requestedReport)
                                      .lexically_normal();
                if (requestedReport.has_parent_path())
                    std::filesystem::create_directories(
                        requestedReport.parent_path());
                std::filesystem::copy_file(
                    result.reportPath, requestedReport,
                    std::filesystem::copy_options::overwrite_existing);
            }
            const nlohmann::json summary = validationSummaryJson(result);
            if (progressNdjson) {
                std::cout
                    << nlohmann::json(
                           {{"event", "started"},
                            {"protocolVersion", 1},
                            {"assetKind", "SceneValidation"},
                            {"total", 1},
                            {"workers", 1}})
                           .dump()
                    << '\n'
                    << nlohmann::json(
                           {{"event", "validation"},
                            {"validation", summary}})
                           .dump()
                    << '\n'
                    << nlohmann::json(
                           {{"event", "completed"},
                            {"assetKind", "SceneValidation"},
                            {"completed", 1},
                            {"encoded", 0},
                            {"reused", result.reused ? 1 : 0},
                            {"failed", 0},
                            {"manifest",
                             result.reportPath.generic_string()}})
                           .dump()
                    << '\n'
                    << std::flush;
            } else {
                std::cout
                    << "glTF validation: "
                    << vkr::assetValidationStateName(result.report.state)
                    << "\n  validator: " << result.report.validatorVersion
                    << "\n  errors: " << result.report.errorCount
                    << "\n  warnings: " << result.report.warningCount
                    << "\n  infos: " << result.report.infoCount
                    << "\n  hints: " << result.report.hintCount
                    << "\n  report: " << result.reportPath.string()
                    << '\n';
                if (!result.report.failureReason.empty())
                    std::cout << "  reason: "
                              << result.report.failureReason << '\n';
            }
            return result.report.state ==
                           vkr::AssetValidationState::Failed
                       ? EXIT_FAILURE
                       : EXIT_SUCCESS;
        }

        if (std::string(argv[1]) == "catalog" &&
            std::string(argv[2]) == "add-environment") {
            std::optional<std::filesystem::path> explicitProject;
            std::filesystem::path source;
            std::string environmentId;
            std::string displayName;
            std::string profileId = "ibl_desktop_v1";
            for (int i = 3; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--source") {
                    source = requireValue(i, argc, argv, argument);
                } else if (argument == "--environment-id") {
                    environmentId =
                        requireValue(i, argc, argv, argument);
                } else if (argument == "--display-name") {
                    displayName = requireValue(i, argc, argv, argument);
                } else if (argument == "--profile") {
                    profileId = requireValue(i, argc, argv, argument);
                } else {
                    throw std::invalid_argument("unknown option: " +
                                                argument);
                }
            }
            if (source.empty())
                throw std::invalid_argument("--source is required");
            source = std::filesystem::absolute(source).lexically_normal();
            std::string sourceExtension = source.extension().string();
            std::transform(sourceExtension.begin(), sourceExtension.end(),
                           sourceExtension.begin(), [](char value) {
                               return static_cast<char>(std::tolower(
                                   static_cast<unsigned char>(value)));
                           });
            if (!std::filesystem::is_regular_file(source) ||
                sourceExtension != ".hdr") {
                throw std::invalid_argument(
                    "--source must name an existing .hdr file");
            }
            if (environmentId.empty())
                environmentId =
                    stableIdFromStem(source.stem().string());
            if (!vkr::isStableAssetId(environmentId))
                throw std::invalid_argument(
                    "--environment-id is not a stable lowercase ID");
            if (displayName.empty())
                displayName = source.stem().string();

            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            (void)catalog.environmentProfile(profileId);
            const std::filesystem::path relative =
                std::filesystem::path("assets/environments") /
                environmentId / source.filename();
            const std::filesystem::path destination =
                (project.projectRoot / relative).lexically_normal();
            if (!vkr::pathIsWithin(project.projectRoot, destination))
                throw std::runtime_error(
                    "environment destination escapes the project");
            if (catalog.findEnvironment(environmentId)) {
                throw std::runtime_error(
                    "environment ID already exists in the Catalog");
            }
            if (std::filesystem::exists(destination)) {
                throw std::runtime_error(
                    "environment destination already exists: " +
                    destination.string());
            }
            std::filesystem::create_directories(destination.parent_path());
            std::error_code copyError;
            std::filesystem::copy_file(
                source, destination,
                std::filesystem::copy_options::none,
                copyError);
            if (copyError)
                throw std::runtime_error(
                    "could not copy environment: " +
                    copyError.message());
            try {
                vkr::CatalogEnvironment environment;
                environment.id = environmentId;
                environment.displayName = displayName;
                environment.source = relative;
                environment.environmentProfile = profileId;
                vkr::SceneCatalogEditor::addEnvironment(project,
                                                        environment);
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(destination, ignored);
                throw;
            }
            std::cout << "Environment imported\n"
                      << "  id: " << environmentId << "\n"
                      << "  name: " << displayName << "\n"
                      << "  source: " << relative.generic_string() << '\n';
            return EXIT_SUCCESS;
        }

        if (std::string(argv[1]) == "environment-cache" &&
            std::string(argv[2]) == "build") {
            std::optional<std::filesystem::path> explicitProject;
            std::filesystem::path cacheRoot;
            std::string environmentId;
            std::string profileId;
            bool force = false;
            bool progressNdjson = false;
            uint32_t workers = 0;
            for (int i = 3; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--cache-root") {
                    cacheRoot = requireValue(i, argc, argv, argument);
                } else if (argument == "--environment-id") {
                    environmentId =
                        requireValue(i, argc, argv, argument);
                } else if (argument == "--profile") {
                    profileId = requireValue(i, argc, argv, argument);
                } else if (argument == "--force") {
                    force = true;
                } else if (argument == "--workers") {
                    workers = parsePositiveUint32(
                        requireValue(i, argc, argv, argument), argument);
                } else if (argument == "--progress-json") {
                    progressNdjson = true;
                } else if (argument == "--progress") {
                    const std::string mode =
                        requireValue(i, argc, argv, argument);
                    if (mode != "ndjson") {
                        throw std::invalid_argument(
                            "--progress currently accepts only ndjson");
                    }
                    progressNdjson = true;
                } else {
                    throw std::invalid_argument("unknown option: " +
                                                argument);
                }
            }
            if (environmentId.empty())
                throw std::invalid_argument(
                    "--environment-id is required");
            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            const vkr::CatalogEnvironment *environment =
                catalog.findEnvironment(environmentId);
            if (!environment)
                throw std::invalid_argument(
                    "unknown Catalog environment: " + environmentId);
            if (profileId.empty())
                profileId = environment->environmentProfile;
            if (cacheRoot.empty()) {
                cacheRoot = vkr::DerivedAssetPaths::defaultCacheRoot(
                    catalog.projectId);
            }
            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            vkr::CacheMutationLock mutationLock(cacheRoot,
                                                &cancelRequested);
            vkr::assettool::EnvironmentCacheBuildOptions options;
            options.source =
                project.projectRoot / environment->source;
            options.sourceProjectPath = environment->source;
            options.cacheRoot = cacheRoot;
            options.projectId = catalog.projectId;
            options.environmentId = environment->id;
            options.profile = catalog.environmentProfile(profileId);
            options.force = force;
            options.maxWorkers = workers;
            options.cancelRequested = &cancelRequested;
            if (progressNdjson) {
                std::cout
                    << nlohmann::json(
                           {{"event", "started"},
                            {"protocolVersion", 1},
                            {"assetKind", "Environment"},
                            {"total", 4},
                            {"workers", workers}})
                           .dump()
                    << '\n'
                    << std::flush;
            }
            const auto report =
                vkr::assettool::buildEnvironmentCache(options);
            bool rebuilt = false;
            vkr::ArtifactIndex index = vkr::ArtifactIndex::loadOrRebuild(
                cacheRoot, project.projectRoot, catalog, &rebuilt);
            index.refreshEnvironment(catalog, environmentId, profileId);
            index.save();
            if (progressNdjson) {
                std::cout
                    << nlohmann::json(
                           {{"event", "completed"},
                            {"assetKind", "Environment"},
                            {"completed", 4},
                            {"encoded", report.generatedBlobs},
                            {"reused", report.reusedBlobs},
                            {"failed", 0},
                            {"manifest",
                             report.manifestPath.generic_string()}})
                           .dump()
                    << '\n'
                    << std::flush;
            } else {
                std::cout
                    << "Environment cache built\n"
                    << "  manifest: " << report.manifestPath.string()
                    << "\n  generated blobs: " << report.generatedBlobs
                    << "\n  reused blobs: " << report.reusedBlobs
                    << "\n  blob bytes: " << report.blobBytes << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (std::string(argv[1]) == "catalog" &&
            std::string(argv[2]) == "add") {
            std::optional<std::filesystem::path> explicitProject;
            vkr::ModelImportRequest request;
            std::filesystem::path cacheRoot;
            std::filesystem::path validatorPath;
            bool allowUnvalidated = false;
            bool forceValidation = false;
            bool hasSource = false;
            for (int i = 3; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--source") {
                    request.sourcePath = requireValue(i, argc, argv, argument);
                    hasSource = true;
                } else if (argument == "--display-name") {
                    request.displayName = requireValue(i, argc, argv, argument);
                } else if (argument == "--model-id" ||
                           argument == "--scene-id") {
                    request.modelId = requireValue(i, argc, argv, argument);
                } else if (argument == "--profile") {
                    request.profileId = requireValue(i, argc, argv, argument);
                } else if (argument == "--cache-root") {
                    cacheRoot = requireValue(i, argc, argv, argument);
                } else if (argument == "--gltf-validator") {
                    validatorPath = requireValue(i, argc, argv, argument);
                } else if (argument == "--allow-unvalidated") {
                    allowUnvalidated = true;
                } else if (argument == "--force") {
                    forceValidation = true;
                } else if (argument == "--reference") {
                    request.placement =
                        vkr::ModelImportPlacement::ReferenceExisting;
                } else {
                    throw std::invalid_argument("unknown option: " + argument);
                }
            }
            if (!hasSource)
                throw std::invalid_argument("--source is required");
            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            if (cacheRoot.empty())
                cacheRoot = vkr::DerivedAssetPaths::defaultCacheRoot(
                    catalog.projectId);
            project.cacheRoot =
                std::filesystem::absolute(cacheRoot).lexically_normal();
            const vkr::ModelImportPreflight preflight =
                vkr::ModelImportService::preflight(request.sourcePath);
            if (request.displayName.empty())
                request.displayName = preflight.suggestedDisplayName;
            if (request.modelId.empty())
                request.modelId = preflight.suggestedModelId;
            if (request.profileId.empty())
                request.profileId = catalog.defaultImportProfile;
            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            vkr::assettool::Win32JobProcessRunner processRunner;
            vkr::assettool::GltfValidationOptions validationOptions;
            validationOptions.sourcePath = request.sourcePath;
            validationOptions.cacheRoot = cacheRoot;
            validationOptions.validatorPath = validatorPath;
            validationOptions.force = forceValidation;
            const auto validation = vkr::assettool::validateGltf(
                validationOptions, cancelRequested, processRunner);
            const bool accepted =
                validation.report.state ==
                    vkr::AssetValidationState::Valid ||
                validation.report.state ==
                    vkr::AssetValidationState::Warnings;
            const bool unavailableBypass =
                validation.report.state ==
                    vkr::AssetValidationState::Unavailable &&
                allowUnvalidated;
            if (!accepted && !unavailableBypass) {
                throw std::runtime_error(
                    "Model validation rejected catalog add: " +
                    std::string(vkr::assetValidationStateName(
                        validation.report.state)) +
                    (validation.report.failureReason.empty()
                         ? std::string{}
                         : " (" + validation.report.failureReason + ")"));
            }
            request.validation =
                vkr::sceneValidationReceipt(validation.report);
            request.allowUnvalidated = unavailableBypass;
            const vkr::ModelImportResult result =
                vkr::ModelImportService::importModel(
                    project, request, {},
                    [](const vkr::ModelImportProgress &progress) {
                        std::cout << "Copied " << progress.completedBytes << '/'
                                  << progress.totalBytes
                                  << " bytes: " << progress.currentFile << '\n';
                    });
            std::cout << "Model imported\n"
                      << "  id: " << result.model.id << "\n"
                      << "  name: " << result.model.displayName << "\n"
                      << "  source: " << result.model.source.generic_string()
                      << "\n  validation: "
                      << vkr::assetValidationStateName(
                             validation.report.state)
                      << "\n  report: " << validation.reportPath.string()
                      << '\n';
            return EXIT_SUCCESS;
        }

        if (std::string(argv[1]) == "package" &&
            std::string(argv[2]) == "verify") {
            std::filesystem::path packageRoot;
            for (int i = 3; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--path") {
                    packageRoot = requireValue(i, argc, argv, argument);
                } else if (argument == "--help") {
                    printUsage(std::cout);
                    return EXIT_SUCCESS;
                } else {
                    throw std::invalid_argument("unknown option: " + argument);
                }
            }
            if (packageRoot.empty())
                throw std::invalid_argument("--path is required");
            packageRoot =
                std::filesystem::absolute(packageRoot).lexically_normal();
            vkr::RuntimePackageManifest manifest;
            std::string error;
            if (!vkr::loadRuntimePackageManifest(
                    packageRoot / "package_manifest.json", manifest, error)) {
                throw std::runtime_error("package manifest is invalid: " +
                                         error);
            }
            const vkr::RuntimePackageVerification verified =
                vkr::verifyRuntimePackage(packageRoot, manifest);
            std::cout << "Package verified\n"
                      << "  path: " << packageRoot.string()
                      << "\n  project: " << manifest.projectId
                      << "\n  schema: " << manifest.schemaVersion
                      << "\n  default profile: "
                      << manifest.defaultImportProfile;
            if (manifest.schemaVersion >=
                vkr::RuntimePackageManifest::kSchemaVersion) {
                std::cout << "\n  scenes: " << manifest.sceneIds.size()
                          << "\n  startup scene: "
                          << manifest.startupSceneId;
            }
            std::cout
                      << "\n  files: " << verified.fileCount
                      << "\n  bytes: " << verified.totalBytes << '\n';
            return EXIT_SUCCESS;
        }

        const bool cacheIndexRebuild =
            argc >= 4 && std::string(argv[1]) == "cache" &&
            std::string(argv[2]) == "index" &&
            std::string(argv[3]) == "rebuild";
        const bool cachePrune = std::string(argv[1]) == "cache" &&
                                std::string(argv[2]) == "prune";
        if (cacheIndexRebuild || cachePrune) {
            std::optional<std::filesystem::path> explicitProject;
            std::filesystem::path cacheRoot;
            uint32_t olderThanDays = 7;
            bool execute = false;
            const int firstOption = cacheIndexRebuild ? 4 : 3;
            for (int i = firstOption; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--cache-root") {
                    cacheRoot = requireValue(i, argc, argv, argument);
                } else if (cachePrune && argument == "--older-than-days") {
                    olderThanDays = parseNonnegativeUint32(
                        requireValue(i, argc, argv, argument), argument);
                } else if (cachePrune && argument == "--execute") {
                    execute = true;
                } else if (argument == "--help") {
                    printUsage(std::cout);
                    return EXIT_SUCCESS;
                } else {
                    throw std::invalid_argument("unknown option: " + argument);
                }
            }
            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            if (cacheRoot.empty())
                cacheRoot =
                    vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId);

            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            if (cacheIndexRebuild) {
                vkr::CacheMutationLock mutationLock(cacheRoot,
                                                    &cancelRequested);
                vkr::ArtifactIndex index = vkr::ArtifactIndex::rebuild(
                    cacheRoot, project.projectRoot, catalog);
                index.save();
                const vkr::ArtifactIndexUsage usage = index.usage();
                std::cout << "Artifact index rebuilt\n"
                          << "  path: " << index.path().string() << "\n"
                          << "  records: " << usage.records << "\n"
                          << "  ready: " << usage.readyRecords << "\n"
                          << "  referenced blobs: " << usage.referencedBlobs
                          << '\n';
                return EXIT_SUCCESS;
            }

            const vkr::ArtifactPruneReport report = vkr::pruneArtifactCache(
                {cacheRoot, olderThanDays, execute, &cancelRequested});
            std::cout << (execute ? "Cache prune complete" :
                                      "Cache prune dry-run")
                      << "\n  manifests: " << report.manifestFiles
                      << "\n  protected blobs: " << report.protectedBlobs
                      << "\n  scanned blobs: " << report.scannedBlobFiles
                      << "\n  candidates: " << report.candidates.size()
                      << "\n  deferred recent blobs: "
                      << report.deferredBlobFiles
                      << "\n  deleted blobs: " << report.deletedBlobFiles
                      << "\n  deleted bytes: " << report.deletedBlobBytes
                      << "\n  validation reports: "
                      << report.scannedValidationReports
                      << "\n  protected validation reports: "
                      << report.protectedValidationReports
                      << "\n  deferred validation reports: "
                      << report.deferredValidationReports
                      << "\n  deleted validation reports: "
                      << report.deletedValidationReports
                      << '\n';
            for (const auto &candidate : report.candidates)
                std::cout << "  candidate: " << candidate.path.string()
                          << " (" << candidate.bytes << " bytes, age "
                           << candidate.ageSeconds << " s)\n";
            for (const auto &candidate : report.validationCandidates)
                std::cout << "  validation candidate: "
                          << candidate.path.string() << " ("
                          << candidate.bytes << " bytes, age "
                          << candidate.ageSeconds << " s)\n";
            if (!execute &&
                (!report.candidates.empty() ||
                 !report.validationCandidates.empty()))
                std::cout << "Re-run with --execute to remove candidates.\n";
            return EXIT_SUCCESS;
        }

        if (std::string(argv[1]) == "cook") {
            std::optional<std::filesystem::path> explicitProject;
            vkr::assettool::CookPackageOptions cook;
            vkr::assettool::TextureCacheBuildOptions build;
            std::filesystem::path validatorPath;
            bool buildMissing = false;
            for (int i = 2; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--project") {
                    explicitProject = requireValue(i, argc, argv, argument);
                } else if (argument == "--cache-root") {
                    cook.cacheRoot = requireValue(i, argc, argv, argument);
                } else if (argument == "--runtime-dir") {
                    cook.runtimeDirectory =
                        requireValue(i, argc, argv, argument);
                } else if (argument == "--output") {
                    cook.outputDirectory =
                        requireValue(i, argc, argv, argument);
                } else if (argument == "--platform") {
                    cook.platform = requireValue(i, argc, argv, argument);
                } else if (argument == "--scene-id") {
                    cook.sceneDocumentIds.push_back(
                        requireValue(i, argc, argv, argument));
                } else if (argument == "--startup-scene") {
                    cook.startupSceneId =
                        requireValue(i, argc, argv, argument);
                } else if (argument == "--model-id" ||
                           argument == "--environment-id" ||
                           argument == "--profile") {
                    throw std::invalid_argument(
                        argument +
                        " is no longer accepted by cook; select native "
                        "SceneDocument roots with --scene-id and use Catalog "
                        "asset profiles");
                } else if (argument == "--build-missing") {
                    buildMissing = true;
                } else if (argument == "--ktx-tool") {
                    build.ktxTool = requireValue(i, argc, argv, argument);
                } else if (argument == "--gltf-validator") {
                    validatorPath = requireValue(i, argc, argv, argument);
                } else if (argument == "--workers") {
                    build.maxWorkers = parsePositiveUint32(
                        requireValue(i, argc, argv, argument), argument);
                } else if (argument == "--memory-budget-mib") {
                    build.memoryBudgetMiB = parsePositiveUint32(
                        requireValue(i, argc, argv, argument), argument);
                } else if (argument == "--help") {
                    printUsage(std::cout);
                    return EXIT_SUCCESS;
                } else {
                    throw std::invalid_argument("unknown option: " + argument);
                }
            }
            if (cook.outputDirectory.empty())
                throw std::invalid_argument("--output is required for cook");
            vkr::ProjectContext project =
                vkr::ProjectContextResolver::resolve(explicitProject);
            const vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
                project.catalogPath, project.projectRoot);
            cook.projectRoot = project.projectRoot;
            if (cook.cacheRoot.empty()) {
                cook.cacheRoot = vkr::DerivedAssetPaths::defaultCacheRoot(
                    catalog.projectId);
            }
            if (cook.runtimeDirectory.empty()) {
                cook.runtimeDirectory =
                    vkr::ProjectContextResolver::currentExecutablePath()
                        .parent_path();
            }

            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            vkr::assettool::Win32JobProcessRunner validatorRunner;
            const vkr::assettool::CookClosure closure =
                vkr::assettool::resolveCookClosure(
                    catalog, project.projectRoot,
                    cook.sceneDocumentIds, cook.startupSceneId);
            for (const vkr::CatalogModel *scene : closure.models) {
                vkr::assettool::GltfValidationOptions validationOptions;
                validationOptions.sourcePath =
                    project.projectRoot / scene->source;
                validationOptions.cacheRoot = cook.cacheRoot;
                validationOptions.validatorPath = validatorPath;
                validationOptions.requireExecutable = true;
                const auto validation = vkr::assettool::validateGltf(
                    validationOptions, cancelRequested, validatorRunner);
                if (validation.report.state !=
                        vkr::AssetValidationState::Valid &&
                    validation.report.state !=
                        vkr::AssetValidationState::Warnings) {
                    throw std::runtime_error(
                        "Cook rejected model '" + scene->id +
                        "' because validation is " +
                        vkr::assetValidationStateName(
                            validation.report.state) +
                        (validation.report.failureReason.empty()
                             ? std::string{}
                             : ": " +
                                   validation.report.failureReason));
                }
                (void)vkr::bindSceneValidation(
                    cook.cacheRoot, project.projectRoot, scene->id,
                    scene->source,
                    vkr::sceneValidationReceipt(validation.report));
            }
            if (buildMissing) {
                for (const vkr::CatalogModel *scene : closure.models) {
                    const vkr::ImportProfile &profile =
                        catalog.profile(scene->importProfile);
                    const std::filesystem::path source =
                        project.projectRoot / scene->source;
                    const vkr::ArtifactStatus status =
                        vkr::inspectTextureArtifacts(
                            {cook.cacheRoot, source, catalog.projectId,
                             scene->id, profile.id, profile.textureLimit,
                             *vkr::textureEncoderFromName(
                                 profile.textureEncoder)});
                    if (status.ready())
                        continue;
                    build.scene = source;
                    build.sceneProjectPath = scene->source;
                    build.cacheRoot = cook.cacheRoot;
                    build.projectId = catalog.projectId;
                    build.sceneId = scene->id;
                    build.profileId = profile.id;
                    build.textureLimit = profile.textureLimit;
                    build.qualityPreset = profile.qualityPreset;
                    build.textureEncoder =
                        *vkr::textureEncoderFromName(profile.textureEncoder);
                    build.cancelRequested = &cancelRequested;
                    vkr::CacheMutationLock buildLock(cook.cacheRoot,
                                                     &cancelRequested);
                    const int buildResult =
                        vkr::assettool::buildTextureCache(build);
                    if (buildResult != 0)
                        return buildResult;
                }
                for (const vkr::CatalogEnvironment *environment :
                     closure.environments) {
                    const std::filesystem::path source =
                        project.projectRoot / environment->source;
                    const vkr::ArtifactStatus status =
                        vkr::inspectEnvironmentArtifacts(
                            {cook.cacheRoot, source, catalog.projectId,
                             environment->id,
                             environment->environmentProfile});
                    if (status.ready())
                        continue;
                    vkr::assettool::EnvironmentCacheBuildOptions
                        environmentBuild;
                    environmentBuild.source = source;
                    environmentBuild.sourceProjectPath =
                        environment->source;
                    environmentBuild.cacheRoot = cook.cacheRoot;
                    environmentBuild.projectId = catalog.projectId;
                    environmentBuild.environmentId = environment->id;
                    environmentBuild.profile =
                        catalog.environmentProfile(
                            environment->environmentProfile);
                    environmentBuild.maxWorkers = build.maxWorkers;
                    environmentBuild.cancelRequested =
                        &cancelRequested;
                    vkr::CacheMutationLock environmentLock(
                        cook.cacheRoot, &cancelRequested);
                    vkr::assettool::buildEnvironmentCache(
                        environmentBuild);
                }
            }

            vkr::CacheMutationLock cookLock(cook.cacheRoot,
                                            &cancelRequested);
            const vkr::assettool::CookPackageReport report =
                vkr::assettool::buildCookPackage(cook);
            std::cout << "Cook complete\n"
                      << "  output: " << report.outputDirectory.string()
                      << "\n  scenes: " << report.sceneCount
                      << "\n  models: " << report.modelCount
                      << "\n  environments: " << report.environmentCount
                      << "\n  manifests: " << report.manifestCount
                      << "\n  unique blobs: " << report.blobCount
                      << "\n  package files: " << report.fileCount
                      << "\n  package bytes: " << report.totalBytes << '\n';
            return EXIT_SUCCESS;
        }

        const bool importModelCommand =
            std::string(argv[1]) == "import" &&
            (std::string(argv[2]) == "model" ||
             std::string(argv[2]) == "scene");
        const bool textureCacheCommand =
            std::string(argv[1]) == "texture-cache";
        if (!importModelCommand && !textureCacheCommand) {
            printUsage(std::cerr);
            return 2;
        }

        const std::string operation =
            importModelCommand ? std::string("build") : std::string(argv[2]);
        if (operation != "build" && operation != "migrate") {
            printUsage(std::cerr);
            return 2;
        }

        vkr::assettool::TextureCacheBuildOptions options;
        vkr::assettool::TextureCacheMigrationOptions migration;
        std::optional<std::filesystem::path> explicitProject;
        std::string sceneId;
        std::string profileId;
        bool hasScene = false;
        bool hasTextureLimit = false;
        bool hasPreset = false;
        bool hasLegacyCacheRoot = false;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--project") {
                explicitProject = requireValue(i, argc, argv, argument);
            } else if (argument == "--scene") {
                options.scene = requireValue(i, argc, argv, argument);
                hasScene = true;
            } else if (argument == "--model-id" ||
                       argument == "--scene-id") {
                sceneId = requireValue(i, argc, argv, argument);
            } else if (argument == "--profile") {
                profileId = requireValue(i, argc, argv, argument);
            } else if (argument == "--texture-limit") {
                options.textureLimit =
                    parseTextureLimit(requireValue(i, argc, argv, argument));
                hasTextureLimit = true;
            } else if (argument == "--cache-root") {
                options.cacheRoot = requireValue(i, argc, argv, argument);
                migration.cacheRoot = options.cacheRoot;
            } else if (argument == "--legacy-cache-root") {
                migration.legacyCacheRoot =
                    requireValue(i, argc, argv, argument);
                hasLegacyCacheRoot = true;
            } else if (argument == "--ktx-tool") {
                options.ktxTool = requireValue(i, argc, argv, argument);
            } else if (argument == "--force") {
                options.force = true;
            } else if (argument == "--workers") {
                options.maxWorkers = parsePositiveUint32(
                    requireValue(i, argc, argv, argument), argument);
            } else if (argument == "--memory-budget-mib") {
                options.memoryBudgetMiB = parsePositiveUint32(
                    requireValue(i, argc, argv, argument), argument);
            } else if (argument == "--preset") {
                options.qualityPreset = requireValue(i, argc, argv, argument);
                hasPreset = true;
                if (options.qualityPreset != "development" &&
                    options.qualityPreset != "production") {
                    throw std::invalid_argument(
                        "--preset must be development or production");
                }
            } else if (argument == "--progress-json") {
                options.progressNdjson = true;
            } else if (argument == "--progress") {
                const std::string mode = requireValue(i, argc, argv, argument);
                if (mode != "ndjson")
                    throw std::invalid_argument(
                        "--progress currently accepts only ndjson");
                options.progressNdjson = true;
            } else if (argument == "--help") {
                printUsage(std::cout);
                return EXIT_SUCCESS;
            } else {
                throw std::invalid_argument("unknown option: " + argument);
            }
        }

        vkr::ProjectContext project =
            vkr::ProjectContextResolver::resolve(explicitProject);
        const vkr::SceneCatalog catalog =
            vkr::SceneCatalog::load(project.catalogPath, project.projectRoot);
        project.cacheRoot =
            vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId);

        if (operation == "migrate") {
            if (!hasLegacyCacheRoot)
                throw std::invalid_argument(
                    "--legacy-cache-root is required for migrate");
            migration.projectRoot = project.projectRoot;
            if (migration.cacheRoot.empty())
                migration.cacheRoot = project.cacheRoot;
            std::atomic_bool cancelRequested{false};
            ConsoleCancellationHandler cancellationHandler(cancelRequested);
            vkr::CacheMutationLock mutationLock(migration.cacheRoot,
                                                &cancelRequested);
            const int result = vkr::assettool::migrateTextureCache(migration);
            if (result == 0) {
                vkr::ArtifactIndex index = vkr::ArtifactIndex::rebuild(
                    migration.cacheRoot, project.projectRoot, catalog);
                index.save();
            }
            return result;
        }

        const vkr::CatalogModel *scene = nullptr;
        if (!sceneId.empty()) {
            scene = catalog.findModel(sceneId);
        } else if (hasScene) {
            const auto requested = options.scene.lexically_normal();
            for (const auto &candidate : catalog.models) {
                if (candidate.type == "gltf" &&
                    candidate.source.lexically_normal() == requested) {
                    scene = &candidate;
                    break;
                }
            }
        }
        if (!scene)
            throw std::invalid_argument(
                "--model-id must name a glTF model in assets/catalog.json");
        if (scene->type != "gltf")
            throw std::invalid_argument("builtin scenes have no texture cache");
        if (profileId.empty()) {
            const vkr::ImportProfile &sceneProfile =
                catalog.profile(scene->importProfile);
            if (!hasTextureLimit ||
                sceneProfile.textureLimit == options.textureLimit) {
                profileId = sceneProfile.id;
            } else {
                for (const auto &candidate : catalog.importProfiles) {
                    if (candidate.second.textureLimit == options.textureLimit) {
                        profileId = candidate.first;
                        break;
                    }
                }
            }
        }
        if (profileId.empty())
            throw std::invalid_argument(
                "no Catalog profile matches --texture-limit");
        const vkr::ImportProfile &profile = catalog.profile(profileId);
        if (hasTextureLimit && profile.textureLimit != options.textureLimit)
            throw std::invalid_argument(
                "--texture-limit does not match the selected profile");

        options.scene = project.projectRoot / scene->source;
        options.sceneProjectPath = scene->source;
        options.projectId = catalog.projectId;
        options.sceneId = scene->id;
        options.profileId = profile.id;
        if (!hasTextureLimit)
            options.textureLimit = profile.textureLimit;
        if (!hasPreset)
            options.qualityPreset = profile.qualityPreset;
        options.textureEncoder =
            *vkr::textureEncoderFromName(profile.textureEncoder);
        if (options.cacheRoot.empty())
            options.cacheRoot = project.cacheRoot;

        std::atomic_bool cancelRequested{false};
        ConsoleCancellationHandler cancellationHandler(cancelRequested);
        options.cancelRequested = &cancelRequested;
        vkr::CacheMutationLock mutationLock(options.cacheRoot,
                                            &cancelRequested);
        const int result = vkr::assettool::buildTextureCache(options);
        if (result == 0) {
            bool rebuilt = false;
            vkr::ArtifactIndex index = vkr::ArtifactIndex::loadOrRebuild(
                options.cacheRoot, project.projectRoot, catalog, &rebuilt);
            index.refresh(catalog, scene->id, profile.id);
            index.save();
        }
        return result;
    } catch (const std::invalid_argument &exception) {
        std::cerr << "Argument error: " << exception.what() << "\n\n";
        printUsage(std::cerr);
        return 2;
    } catch (const std::exception &exception) {
        std::cerr << "Asset tool failed: " << exception.what() << '\n';
        return 1;
    }
}
