#include "app/Application.h"
#include "core/Log.h"
#include "scene/BuiltinScenes.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void printUsage(std::ostream &out) {
    out << "Usage: VulkanLab.exe [--runtime-control] [--help]\n"
        << "\n"
        << "Options:\n"
        << "  --runtime-control  Enable the local VulkanLabCtl named-pipe "
           "interface.\n"
        << "  --help             Show this help and exit.\n";
}

bool parseArguments(int argc, char **argv, vkr::Config &config) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--runtime-control") {
            config.enableRuntimeControl = true;
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

        vkr::Application app(config);

        app.registerScene({"Viking Room",
                           vkr::vikingRoomSceneFactory(config.texturePath,
                                                       config.vertShaderPath,
                                                       config.fragShaderPath)});
        app.registerScene({"Sheen Chair",
                           vkr::sheenChairSceneFactory(config.vertShaderPath,
                                                        config.fragShaderPath)});

        auto registerOptionalGltf =
            [&](const char *name, const char *path,
                std::optional<vkr::CameraPose> cameraOverride = std::nullopt) {
            if (!std::filesystem::exists(path)) {
                VKR_LOG_INFO("App", "Skipping optional scene '{}': missing {}",
                             name, path);
                return;
            }
            app.registerScene(
                {name, vkr::gltfSceneFactory(path, config.vertShaderPath,
                                             config.fragShaderPath,
                                             cameraOverride)});
        };

        registerOptionalGltf("A Beautiful Game",
                             "models/ABeautifulGame.glb");
        registerOptionalGltf("Anisotropy Barn Lamp",
                             "models/AnisotropyBarnLamp.glb");
        registerOptionalGltf("Car Concept", "models/CarConcept.glb");
        registerOptionalGltf("Chronograph Watch",
                             "models/ChronographWatch.glb");
        registerOptionalGltf("Diffuse Transmission Teacup",
                             "models/DiffuseTransmissionTeacup.glb");
        registerOptionalGltf("Pot of Coals", "models/PotOfCoals.glb");
        registerOptionalGltf(
            "Main Sponza", "models/main_sponza/NewSponza_Main_glTF_003.gltf",
            vkr::CameraPose{{0.0f, -35.0f, 10.0f}, 93.0f, -2.0f});

        app.run();
    } catch (const std::exception &e) {
        VKR_LOG_CRITICAL("App", "{}", e.what());
        vkr::log::shutdown();
        return EXIT_FAILURE;
    }

    vkr::log::shutdown();
    return EXIT_SUCCESS;
}
