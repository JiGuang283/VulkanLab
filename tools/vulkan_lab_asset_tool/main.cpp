#include "TextureCacheBuilder.h"

#include "assets/DerivedAssetPaths.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "assets/SceneImportService.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

void printUsage(std::ostream &output) {
    output
        << "Usage:\n"
        << "  VulkanLabAssetTool texture-cache build --scene-id <id> "
           "[--profile <id>] [options]\n"
        << "  VulkanLabAssetTool texture-cache migrate "
           "--legacy-cache-root <path> [options]\n\n"
        << "  VulkanLabAssetTool catalog add --source <path> [options]\n\n"
        << "Options:\n"
        << "  --project <path>     Source project root (otherwise use locator)\n"
        << "  --scene <path>       Legacy alias; match a Catalog source path\n"
        << "  --scene-id <id>      Stable Catalog scene ID\n"
        << "  --profile <id>       Import profile (default: scene profile)\n"
        << "  --texture-limit <n>  Override profile limit: 0/512/1024/2048\n"
        << "  --cache-root <path>  Override the shared derived cache root\n"
        << "  --legacy-cache-root  Legacy working-directory cache to migrate\n"
        << "  --force              Re-encode blobs even when they are valid\n"
        << "  --ktx-tool <path>    Path to the KTX 4.4.2 `ktx` executable\n"
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
    static const std::unordered_set<unsigned long> allowed{0, 512, 1024,
                                                           2048};
    if (allowed.count(parsed) == 0)
        throw std::invalid_argument(
            "texture limit must be one of 0, 512, 1024, or 2048");
    return static_cast<uint32_t>(parsed);
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

        if (std::string(argv[1]) == "catalog" &&
            std::string(argv[2]) == "add") {
            std::optional<std::filesystem::path> explicitProject;
            vkr::SceneImportRequest request;
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
                } else if (argument == "--scene-id") {
                    request.sceneId = requireValue(i, argc, argv, argument);
                } else if (argument == "--profile") {
                    request.profileId = requireValue(i, argc, argv, argument);
                } else if (argument == "--reference") {
                    request.placement =
                        vkr::SceneImportPlacement::ReferenceExisting;
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
            const vkr::SceneImportPreflight preflight =
                vkr::SceneImportService::preflight(request.sourcePath);
            if (request.displayName.empty())
                request.displayName = preflight.suggestedDisplayName;
            if (request.sceneId.empty())
                request.sceneId = preflight.suggestedSceneId;
            if (request.profileId.empty())
                request.profileId = catalog.defaultImportProfile;
            const vkr::SceneImportResult result =
                vkr::SceneImportService::importScene(project, request, {},
                    [](const vkr::SceneImportProgress &progress) {
                        std::cout << "Copied " << progress.completedBytes << '/'
                                  << progress.totalBytes << " bytes: "
                                  << progress.currentFile << '\n';
                    });
            std::cout << "Scene imported\n"
                      << "  id: " << result.scene.id << "\n"
                      << "  name: " << result.scene.displayName << "\n"
                      << "  source: " << result.scene.source.generic_string()
                      << '\n';
            return EXIT_SUCCESS;
        }

        if (std::string(argv[1]) != "texture-cache") {
            printUsage(std::cerr);
            return 2;
        }

        const std::string operation = argv[2];
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
        bool hasLegacyCacheRoot = false;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--project") {
                explicitProject = requireValue(i, argc, argv, argument);
            } else if (argument == "--scene") {
                options.scene = requireValue(i, argc, argv, argument);
                hasScene = true;
            } else if (argument == "--scene-id") {
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
        project.cacheRoot =
            vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId);

        if (operation == "migrate") {
            if (!hasLegacyCacheRoot)
                throw std::invalid_argument(
                    "--legacy-cache-root is required for migrate");
            migration.projectRoot = project.projectRoot;
            if (migration.cacheRoot.empty())
                migration.cacheRoot = project.cacheRoot;
            return vkr::assettool::migrateTextureCache(migration);
        }

        const vkr::CatalogScene *scene = nullptr;
        if (!sceneId.empty()) {
            scene = catalog.findScene(sceneId);
        } else if (hasScene) {
            const auto requested = options.scene.lexically_normal();
            for (const auto &candidate : catalog.scenes) {
                if (candidate.type == "gltf" &&
                    candidate.source.lexically_normal() == requested) {
                    scene = &candidate;
                    break;
                }
            }
        }
        if (!scene)
            throw std::invalid_argument(
                "--scene-id must name a glTF scene in assets/catalog.json");
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
        if (options.cacheRoot.empty())
            options.cacheRoot = project.cacheRoot;

        return vkr::assettool::buildTextureCache(options);
    } catch (const std::invalid_argument &exception) {
        std::cerr << "Argument error: " << exception.what() << "\n\n";
        printUsage(std::cerr);
        return 2;
    } catch (const std::exception &exception) {
        std::cerr << "Asset tool failed: " << exception.what() << '\n';
        return 1;
    }
}
