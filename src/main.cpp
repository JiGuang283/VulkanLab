#include "app/Application.h"
#include "scene/BuiltinScenes.h"
#include <iostream>

int main() {
    vkr::Config config;
    // 按需覆写默认配置，例如：
    // config.windowWidth  = 1280;
    // config.windowHeight = 720;

    vkr::Application app(config);

    app.registerScene(
        {"Viking Room",
         vkr::vikingRoomSceneFactory(config.texturePath, config.vertShaderPath,
                                     config.fragShaderPath)});
    app.registerScene(
        {"Sheen Chair",
         vkr::sheenChairSceneFactory(config.texturePath, config.vertShaderPath,
                                     config.fragShaderPath)});

    try {
        app.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}