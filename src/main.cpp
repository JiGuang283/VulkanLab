#include "app/Application.h"
#include "assets/DerivedAssetPaths.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "control/RuntimeControlProtocol.h"
#include "core/Log.h"

#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

std::string utf8Argument(const wchar_t *value) {
    if (!value || *value == L'\0')
        return {};
    const int inputLength = static_cast<int>(std::wcslen(value));
    const int outputLength = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, inputLength, nullptr, 0,
        nullptr, nullptr);
    if (outputLength <= 0)
        throw std::invalid_argument("Command line contains invalid Unicode");
    std::string result(static_cast<size_t>(outputLength), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                            inputLength, result.data(), outputLength,
                            nullptr, nullptr) != outputLength) {
        throw std::invalid_argument("Command line contains invalid Unicode");
    }
    return result;
}

void printUsage(std::ostream &out) {
    out << "Usage: VulkanLab.exe [--project <path>] [--runtime-control] "
           "[--runtime-control-pipe <suffix>] [--validation <profile>] "
           "[--asset-mode <mode>] [--cache-root <path>] [diagnostics] "
           "[--help]\n"
        << "\n"
        << "Options:\n"
        << "  --runtime-control  Enable the local VulkanLabCtl named-pipe "
           "interface.\n"
        << "  --runtime-control-pipe <suffix>  Use "
           "\\\\.\\pipe\\VulkanLab.<suffix>; ASCII letters, digits, '-' "
           "and '_' only.\n"
        << "  --project <path>   Use the source project and writable scene "
           "catalog at <path>.\n"
        << "  --asset-mode <mode>  ondemand, readonly, or cooked-only.\n"
        << "  --asset-tool <path>  Override VulkanLabAssetTool.exe path.\n"
        << "  --cache-root <path>  Override the derived asset cache root.\n"
        << "  --validation <profile>  Use off, core, sync, or gpu "
           "validation (default: core).\n"
        << "  --automation      Enable deterministic automation behavior.\n"
        << "  --window-size <WxH>  Use a fixed, non-resizable window size.\n"
        << "  --fixed-delta <seconds>  Advance scene simulation by a fixed "
           "delta in (0, 1].\n"
        << "  --no-gui          Disable ImGui initialization and rendering.\n"
        << "  --capture-root <path>  Override the diagnostics output root.\n"
        << "  --help             Show this help and exit.\n";
}

bool parseArguments(int argc, wchar_t **argv, vkr::Config &config) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--runtime-control") {
            config.enableRuntimeControl = true;
        } else if (argument == L"--runtime-control-pipe") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--runtime-control-pipe requires a suffix");
            const std::string suffix = utf8Argument(argv[i]);
            if (suffix.empty())
                throw std::invalid_argument(
                    "--runtime-control-pipe requires a non-empty suffix");
            vkr::control::makeRuntimeControlEndpoint(suffix);
            config.diagnostics.runtimePipeSuffix = suffix;
        } else if (argument == L"--automation") {
            config.diagnostics.automationMode = true;
        } else if (argument == L"--window-size") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--window-size requires WIDTHxHEIGHT");
            const vkr::DiagnosticWindowSize size =
                vkr::parseDiagnosticWindowSize(utf8Argument(argv[i]));
            config.windowWidth = size.width;
            config.windowHeight = size.height;
            config.diagnostics.fixedWindowSize = true;
        } else if (argument == L"--fixed-delta") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--fixed-delta requires a value in seconds");
            config.diagnostics.fixedDeltaSeconds =
                vkr::parseDiagnosticFixedDelta(utf8Argument(argv[i]));
        } else if (argument == L"--no-gui") {
            config.diagnostics.guiVisible = false;
        } else if (argument == L"--capture-root") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--capture-root requires a directory path");
            config.diagnostics.captureRoot = argv[i];
        } else if (argument == L"--project") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--project requires a directory path");
            config.projectPath = argv[i];
        } else if (argument == L"--asset-mode") {
            if (++i >= argc)
                throw std::invalid_argument("--asset-mode requires a value");
            const std::string mode = utf8Argument(argv[i]);
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
        } else if (argument == L"--asset-tool") {
            if (++i >= argc)
                throw std::invalid_argument("--asset-tool requires a path");
            config.assetToolPath =
                std::filesystem::path(argv[i]).u8string();
            config.assetToolPathExplicit = true;
        } else if (argument == L"--cache-root") {
            if (++i >= argc)
                throw std::invalid_argument("--cache-root requires a path");
            config.derivedTextureCachePath =
                std::filesystem::path(argv[i]).u8string();
            config.cachePathExplicit = true;
        } else if (argument == L"--validation") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--validation requires a profile");
            config.validationProfile =
                vkr::parseValidationProfile(utf8Argument(argv[i]));
        } else if (argument == L"--help") {
            return false;
        } else {
            throw std::invalid_argument("Unknown argument: " +
                                        utf8Argument(argv[i]));
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
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
            config.gltfMaxTextureSize =
                catalog.profile(projectContext.packageProfileId).textureLimit;
            config.derivedTextureCachePath =
                projectContext.cacheRoot.u8string();
        } else {
            projectContext.cacheRoot = config.derivedTextureCachePath.empty()
                                           ? vkr::DerivedAssetPaths::defaultCacheRoot(
                                                 catalog.projectId)
                                           : std::filesystem::absolute(
                                                 std::filesystem::u8path(
                                                     config.derivedTextureCachePath))
                                                 .lexically_normal();
            config.derivedTextureCachePath =
                projectContext.cacheRoot.u8string();
            if (config.assetToolPathExplicit) {
                config.assetToolPath =
                    std::filesystem::absolute(std::filesystem::u8path(
                                                  config.assetToolPath))
                        .lexically_normal()
                        .u8string();
            }
        }
        if (captureRootExplicit) {
            projectContext.captureRoot =
                std::filesystem::absolute(config.diagnostics.captureRoot)
                    .lexically_normal();
        }
        config.diagnostics.captureRoot = projectContext.captureRoot;
        VKR_LOG_INFO("Assets", "Project '{}' from '{}' ({})", catalog.projectId,
                     projectContext.projectRoot.u8string(),
                     projectContext.diagnostic);
        VKR_LOG_INFO("Assets", "Runtime root: '{}'",
                     projectContext.runtimeRoot.u8string());
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
