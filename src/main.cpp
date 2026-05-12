#include "app/Application.h"
#include "core/Log.h"
#include "scene/BuiltinScenes.h"

#include <cstdlib>
#include <exception>
#include <filesystem>

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

        auto registerOptionalGltf = [&](const char *name, const char *path) {
            if (!std::filesystem::exists(path)) {
                VKR_LOG_INFO("App", "Skipping optional scene '{}': missing {}",
                             name, path);
                return;
            }
            app.registerScene(
                {name, vkr::gltfSceneFactory(path, config.vertShaderPath,
                                             config.fragShaderPath)});
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

        app.run();
    } catch (const std::exception &e) {
        VKR_LOG_CRITICAL("App", "{}", e.what());
        vkr::log::shutdown();
        return EXIT_FAILURE;
    }

    vkr::log::shutdown();
    return EXIT_SUCCESS;
}
