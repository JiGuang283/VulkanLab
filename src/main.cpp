#include "app/Application.h"
#include "assets/DerivedAssetPaths.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "control/RuntimeControlProtocol.h"
#include "diagnostics/BuildInfo.h"
#include "core/Log.h"
#include "render/shader/ShaderRegistry.h"

#include <BuildFeatures.h>
#include <json.hpp>

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
           "[--material-binding <mode>] "
           "[--render-path <mode>] "
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
        << "  --gltf-validator <path>  Override Khronos glTF Validator "
           "2.0.0-dev.3.10 path.\n"
        << "  --cache-root <path>  Override the derived asset cache root.\n"
        << "  --validation <profile>  Use off, core, sync, or gpu "
           "validation (default: core).\n"
        << "  --material-binding <mode>  Use auto, legacy, or bindless "
           "material resources (default: auto).\n"
        << "  --render-path <mode>  Use auto, forward, or deferred "
           "opaque rendering (default: auto).\n"
        << "  --automation      Enable deterministic automation behavior.\n"
        << "  --window-size <WxH>  Use a fixed, non-resizable window size.\n"
        << "  --fixed-delta <seconds>  Advance scene simulation by a fixed "
           "delta in (0, 1].\n"
        << "  --no-gui          Disable ImGui initialization and rendering.\n"
        << "  --capture-root <path>  Override the diagnostics output root.\n"
        << "  --build-info-json  Print machine-readable build information "
           "and exit.\n"
        << "  --help             Show this help and exit.\n"
        << "\nCompiled features:\n"
        << "  editor-ui=" << (vkr::build::kEditorUi ? "on" : "off")
        << ", runtime-control="
        << (vkr::build::kRuntimeControl ? "on" : "off")
        << ", capture=" << (vkr::build::kCapture ? "on" : "off")
        << ", asset-authoring="
        << (vkr::build::kAssetAuthoring ? "on" : "off")
        << ", validation=" << (vkr::build::kValidation ? "on" : "off")
        << ", gpu-debug-utils="
        << (vkr::build::kGpuDebugUtils ? "on" : "off")
        << ", gpu-profiling="
        << (vkr::build::kGpuProfiling ? "on" : "off")
        << ", tracy=" << (vkr::build::kTracy ? "on" : "off")
        << ", cacao=" << (vkr::build::kCacao ? "on" : "off") << "\n";
}

void printBuildInfoJson(std::ostream &out) {
    const vkr::BuildInfo &build = vkr::currentBuildInfo();
    const vkr::BuildFeatureInfo &features = build.features;
    const nlohmann::json root = {
        {"schemaVersion", 1},
        {"revision", build.revision},
        {"dirty", build.dirty},
        {"configuration", build.configuration},
        {"compiler", build.compiler},
        {"vulkanSdk", build.vulkanSdk},
        {"features",
         {{"editorUi", features.editorUi},
          {"runtimeControl", features.runtimeControl},
          {"capture", features.capture},
          {"assetAuthoring", features.assetAuthoring},
          {"validation", features.validation},
          {"gpuDebugUtils", features.gpuDebugUtils},
          {"gpuProfiling", features.gpuProfiling},
          {"tracy", features.tracy},
          {"cacao", features.cacao},
          {"assetTool", features.assetTool},
          {"controlTool", features.controlTool},
          {"renderTest", features.renderTest}}},
        {"rendering",
         {{"renderPaths", {"forward", "deferred"}},
          {"materialBindings", {"legacy", "bindless"}},
          {"shaderManifestSchema", vkr::ShaderRegistry::kSchemaVersion}}}};
    out << root.dump() << '\n';
}

bool parseArguments(int argc, wchar_t **argv, vkr::Config &config) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--runtime-control") {
#if !VKL_ENABLE_RUNTIME_CONTROL
            throw std::invalid_argument(
                "--runtime-control is not compiled into this build");
#else
            config.enableRuntimeControl = true;
#endif
        } else if (argument == L"--runtime-control-pipe") {
#if !VKL_ENABLE_RUNTIME_CONTROL
            throw std::invalid_argument(
                "--runtime-control-pipe is not compiled into this build");
#else
            if (++i >= argc)
                throw std::invalid_argument(
                    "--runtime-control-pipe requires a suffix");
            const std::string suffix = utf8Argument(argv[i]);
            if (suffix.empty())
                throw std::invalid_argument(
                    "--runtime-control-pipe requires a non-empty suffix");
            vkr::control::makeRuntimeControlEndpoint(suffix);
            config.diagnostics.runtimePipeSuffix = suffix;
#endif
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
#if !VKL_ENABLE_CAPTURE
            throw std::invalid_argument(
                "--capture-root is not compiled into this build");
#else
            if (++i >= argc)
                throw std::invalid_argument(
                    "--capture-root requires a directory path");
            config.diagnostics.captureRoot = argv[i];
#endif
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
            if (mode == "ondemand") {
#if !VKL_ENABLE_ASSET_AUTHORING
                throw std::invalid_argument(
                    "--asset-mode ondemand is not compiled into this build");
#else
                config.assetImportMode = vkr::AssetImportMode::OnDemand;
#endif
            } else if (mode == "readonly")
                config.assetImportMode = vkr::AssetImportMode::ReadOnly;
            else if (mode == "cooked-only")
                config.assetImportMode = vkr::AssetImportMode::CookedOnly;
            else
                throw std::invalid_argument(
                    "--asset-mode must be ondemand, readonly, or "
                    "cooked-only");
        } else if (argument == L"--asset-tool") {
#if !VKL_ENABLE_ASSET_AUTHORING
            throw std::invalid_argument(
                "--asset-tool is not compiled into this build");
#else
            if (++i >= argc)
                throw std::invalid_argument("--asset-tool requires a path");
            config.assetToolPath =
                std::filesystem::path(argv[i]).u8string();
            config.assetToolPathExplicit = true;
#endif
        } else if (argument == L"--gltf-validator") {
#if !VKL_ENABLE_ASSET_AUTHORING
            throw std::invalid_argument(
                "--gltf-validator is not compiled into this build");
#else
            if (++i >= argc)
                throw std::invalid_argument(
                    "--gltf-validator requires a path");
            config.gltfValidatorPath =
                std::filesystem::path(argv[i]).u8string();
            config.gltfValidatorPathExplicit = true;
#endif
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
            const vkr::ValidationProfile profile =
                vkr::parseValidationProfile(utf8Argument(argv[i]));
#if !VKL_ENABLE_VALIDATION
            if (profile != vkr::ValidationProfile::Off) {
                throw std::invalid_argument(
                    "Vulkan validation is not compiled into this build; "
                    "only --validation off is accepted");
            }
#endif
            config.validationProfile = profile;
        } else if (argument == L"--material-binding") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--material-binding requires a mode");
            try {
                config.materialBindingMode = vkr::parseMaterialBindingMode(
                    utf8Argument(argv[i]));
            } catch (const char *message) {
                throw std::invalid_argument(message);
            }
        } else if (argument == L"--render-path") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--render-path requires a mode");
            const auto request = vkr::renderPathRequestFromName(
                utf8Argument(argv[i]));
            if (!request) {
                throw std::invalid_argument(
                    "--render-path must be auto, forward, or deferred");
            }
            config.renderPath = *request;
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
    if (argc == 2 && std::wstring(argv[1]) == L"--build-info-json") {
        printBuildInfoJson(std::cout);
        return EXIT_SUCCESS;
    }
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
        const vkr::BuildFeatureInfo &features =
            vkr::currentBuildInfo().features;
        VKR_LOG_INFO(
            "Build",
            "Compiled features: editor-ui={}, runtime-control={}, "
            "capture={}, asset-authoring={}, validation={}, "
            "gpu-debug-utils={}, gpu-profiling={}, tracy={}, cacao={}",
            features.editorUi, features.runtimeControl, features.capture,
            features.assetAuthoring, features.validation,
            features.gpuDebugUtils, features.gpuProfiling, features.tracy,
            features.cacao);
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
            if (config.cachePathExplicit || config.assetToolPathExplicit ||
                config.gltfValidatorPathExplicit)
                throw std::runtime_error(
                    "A cooked package cannot use external cache, asset tool, "
                    "or validator paths");
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
            if (config.gltfValidatorPathExplicit) {
                config.gltfValidatorPath =
                    std::filesystem::absolute(std::filesystem::u8path(
                                                  config.gltfValidatorPath))
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
