#include "app/Application.h"
#include "assets/DerivedAssetPaths.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "core/Log.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void printUsage(std::ostream &out) {
    out << "Usage: VulkanLab.exe [--project <path>] [--runtime-control] "
           "[--asset-mode <mode>] [--cache-root <path>] [diagnostics] "
           "[--help]\n"
        << "\n"
        << "Options:\n"
        << "  --runtime-control  Enable the local VulkanLabCtl named-pipe "
           "interface.\n"
        << "  --project <path>   Use the source project and writable scene "
           "catalog at <path>.\n"
        << "  --asset-mode <mode>  ondemand, readonly, or cooked-only.\n"
        << "  --asset-tool <path>  Override VulkanLabAssetTool.exe path.\n"
        << "  --cache-root <path>  Override the derived asset cache root.\n"
        << "  --automation      Enable deterministic automation behavior.\n"
        << "  --window-size <WxH>  Use a fixed, non-resizable window size.\n"
        << "  --fixed-delta <seconds>  Advance scene simulation by a fixed "
           "delta in (0, 1].\n"
        << "  --no-gui          Disable ImGui initialization and rendering.\n"
        << "  --capture-root <path>  Override the diagnostics output root.\n"
        << "  --help             Show this help and exit.\n";
}

bool parseArguments(int argc, char **argv, vkr::Config &config) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--runtime-control") {
            config.enableRuntimeControl = true;
        } else if (argument == "--automation") {
            config.diagnostics.automationMode = true;
        } else if (argument == "--window-size") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--window-size requires WIDTHxHEIGHT");
            const vkr::DiagnosticWindowSize size =
                vkr::parseDiagnosticWindowSize(argv[i]);
            config.windowWidth = size.width;
            config.windowHeight = size.height;
            config.diagnostics.fixedWindowSize = true;
        } else if (argument == "--fixed-delta") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--fixed-delta requires a value in seconds");
            config.diagnostics.fixedDeltaSeconds =
                vkr::parseDiagnosticFixedDelta(argv[i]);
        } else if (argument == "--no-gui") {
            config.diagnostics.guiVisible = false;
        } else if (argument == "--capture-root") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--capture-root requires a directory path");
            config.diagnostics.captureRoot = argv[i];
        } else if (argument == "--project") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--project requires a directory path");
            config.projectPath = argv[i];
        } else if (argument == "--asset-mode") {
            if (++i >= argc)
                throw std::invalid_argument("--asset-mode requires a value");
            const std::string mode = argv[i];
            config.assetImportModeExplicit = true;
            if (mode == "ondemand")
                config.assetImportMode = vkr::AssetImportMode::OnDemand;
            else if (mode == "readonly")
                config.assetImportMode = vkr::AssetImportMode::ReadOnly;
            else if (mode == "cooked-only")
                config.assetImportMode = vkr::AssetImportMode::CookedOnly;
            else
                throw std::invalid_argument(
                    "--asset-mode must be ondemand, readonly, or "
                    "cooked-only");
        } else if (argument == "--asset-tool") {
            if (++i >= argc)
                throw std::invalid_argument("--asset-tool requires a path");
            config.assetToolPath = argv[i];
            config.assetToolPathExplicit = true;
        } else if (argument == "--cache-root") {
            if (++i >= argc)
                throw std::invalid_argument("--cache-root requires a path");
            config.derivedTextureCachePath = argv[i];
            config.cachePathExplicit = true;
        } else if (argument == "--help") {
            return false;
        } else {
            throw std::invalid_argument("Unknown argument: " + argument);
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    vkr::Config config;
    try {
        if (!parseArguments(argc, argv, config)) {
            printUsage(std::cout);
            return EXIT_SUCCESS;
        }
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        printUsage(std::cerr);
        return EXIT_FAILURE;
    }

    vkr::log::init();
    try {
        // 按需覆写默认配置，例如：
        // config.windowWidth  = 1280;
        // config.windowHeight = 720;

        const std::optional<std::filesystem::path> explicitProject =
            config.projectPath.empty()
                ? std::nullopt
                : std::optional<std::filesystem::path>(config.projectPath);
        vkr::ProjectContext projectContext =
            vkr::ProjectContextResolver::resolve(explicitProject);
        vkr::SceneCatalog catalog = vkr::SceneCatalog::load(
            projectContext.catalogPath, projectContext.projectRoot);
        const bool captureRootExplicit =
            !config.diagnostics.captureRoot.empty();
        if (projectContext.cookedPackage) {
            if (config.cachePathExplicit || config.assetToolPathExplicit)
                throw std::runtime_error(
                    "A cooked package cannot use external cache or asset tool paths");
            if (captureRootExplicit)
                throw std::runtime_error(
                    "A cooked package cannot override the diagnostics capture root");
            if (config.assetImportModeExplicit &&
                config.assetImportMode != vkr::AssetImportMode::CookedOnly) {
                throw std::runtime_error(
                    "A cooked package requires --asset-mode cooked-only");
            }
            if (catalog.defaultImportProfile !=
                projectContext.packageProfileId) {
                throw std::runtime_error(
                    "Cooked catalog profile does not match package manifest");
            }
            config.assetImportMode = vkr::AssetImportMode::CookedOnly;
            config.enableValidation = false;
            config.gltfMaxTextureSize =
                catalog.profile(projectContext.packageProfileId).textureLimit;
            config.derivedTextureCachePath =
                projectContext.cacheRoot.string();
        } else {
            projectContext.cacheRoot = config.derivedTextureCachePath.empty()
                                           ? vkr::DerivedAssetPaths::defaultCacheRoot(
                                                 catalog.projectId)
                                           : std::filesystem::absolute(
                                                 config.derivedTextureCachePath)
                                                 .lexically_normal();
            config.derivedTextureCachePath =
                projectContext.cacheRoot.string();
            if (config.assetToolPathExplicit) {
                config.assetToolPath =
                    std::filesystem::absolute(config.assetToolPath)
                        .lexically_normal()
                        .string();
            }
        }
        if (captureRootExplicit) {
            projectContext.captureRoot =
                std::filesystem::absolute(config.diagnostics.captureRoot)
                    .lexically_normal();
        }
        config.diagnostics.captureRoot = projectContext.captureRoot;
        VKR_LOG_INFO("Assets", "Project '{}' from '{}' ({})", catalog.projectId,
                     projectContext.projectRoot.string(),
                     projectContext.diagnostic);
        VKR_LOG_INFO("Assets", "Runtime root: '{}'",
                     projectContext.runtimeRoot.string());
        VKR_LOG_INFO("Assets", "Derived asset cache: '{}'",
                     config.derivedTextureCachePath);

        vkr::Application app(config, std::move(projectContext),
                             std::move(catalog));

        app.run();
    } catch (const std::exception &e) {
        VKR_LOG_CRITICAL("App", "{}", e.what());
        vkr::log::shutdown();
        return EXIT_FAILURE;
    }

    vkr::log::shutdown();
    return EXIT_SUCCESS;
}
