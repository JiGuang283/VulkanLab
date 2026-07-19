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
           "[--help]\n"
        << "\n"
        << "Options:\n"
        << "  --runtime-control  Enable the local VulkanLabCtl named-pipe "
           "interface.\n"
        << "  --project <path>   Use the source project and writable scene "
           "catalog at <path>.\n"
        << "  --help             Show this help and exit.\n";
}

bool parseArguments(int argc, char **argv, vkr::Config &config) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--runtime-control") {
            config.enableRuntimeControl = true;
        } else if (argument == "--project") {
            if (++i >= argc)
                throw std::invalid_argument(
                    "--project requires a directory path");
            config.projectPath = argv[i];
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
        projectContext.cacheRoot =
            vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId);
        if (config.derivedTextureCachePath.empty())
            config.derivedTextureCachePath = projectContext.cacheRoot.string();
        VKR_LOG_INFO("Assets", "Project '{}' from '{}' ({})", catalog.projectId,
                     projectContext.projectRoot.string(),
                     projectContext.diagnostic);
        VKR_LOG_INFO("Assets", "Derived asset cache: '{}'",
                     projectContext.cacheRoot.string());

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
