#include "app/Application.h"
#include <iostream>

int main() {
    vkr::Config config;
    // 按需覆写默认配置，例如：
    // config.windowWidth  = 1280;
    // config.windowHeight = 720;
    config.modelPath = "models/SheenChair.glb";

    vkr::Application app(config);

    try {
        app.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}