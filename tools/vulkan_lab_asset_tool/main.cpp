#include "TextureCacheBuilder.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

void printUsage(std::ostream &output) {
    output
        << "Usage:\n"
        << "  VulkanLabAssetTool texture-cache build --scene <path> "
           "--texture-limit <0|512|1024|2048> [options]\n\n"
        << "Options:\n"
        << "  --cache-root <path>  Derived cache root (default: derived_assets)\n"
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
        if (argc < 3 || std::string(argv[1]) != "texture-cache" ||
            std::string(argv[2]) != "build") {
            printUsage(std::cerr);
            return 2;
        }

        vkr::assettool::TextureCacheBuildOptions options;
        bool hasScene = false;
        bool hasTextureLimit = false;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--scene") {
                options.scene = requireValue(i, argc, argv, argument);
                hasScene = true;
            } else if (argument == "--texture-limit") {
                options.textureLimit =
                    parseTextureLimit(requireValue(i, argc, argv, argument));
                hasTextureLimit = true;
            } else if (argument == "--cache-root") {
                options.cacheRoot = requireValue(i, argc, argv, argument);
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
        if (!hasScene)
            throw std::invalid_argument("--scene is required");
        if (!hasTextureLimit)
            throw std::invalid_argument("--texture-limit is required");

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
