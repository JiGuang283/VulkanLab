#pragma once

#include <memory>
#include <string>

namespace vkr {

class Texture;

struct EnvironmentGpuResources {
    std::string environmentId;
    std::string displayName;
    std::string profileId;
    std::shared_ptr<Texture> radiance;
    std::shared_ptr<Texture> irradiance;
    std::shared_ptr<Texture> prefilteredSpecular;
    std::shared_ptr<Texture> brdfLut;
    float maxSpecularLod = 0.0f;
};

} // namespace vkr
