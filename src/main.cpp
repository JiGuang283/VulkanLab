#include "app/Application.h"
#include "core/Log.h"
#include "scene/BuiltinScenes.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>

int main() {
    vkr::log::init();
    try {
        vkr::Config config;
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
