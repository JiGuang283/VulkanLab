#pragma once

#include "scene/SceneTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

struct ImportProfile {
    std::string id;
    uint32_t textureLimit = 2048;
    std::string textureEncoder = "bc7";
    std::string qualityPreset = "development";
};

struct EnvironmentProfile {
    std::string id;
    uint32_t radianceSize = 512;
    uint32_t irradianceSize = 32;
    uint32_t prefilteredSize = 256;
    uint32_t brdfLutSize = 256;
    uint32_t diffuseSamples = 1024;
    uint32_t specularSamples = 512;
    uint32_t brdfSamples = 1024;
};

struct CatalogScene {
    std::string id;
    std::string displayName;
    std::string type = "gltf";
    std::string builtinFactory;
    std::filesystem::path source;
    std::string importProfile;
    bool optional = false;
    std::optional<CameraPose> camera;
};

struct CatalogEnvironment {
    std::string id;
    std::string displayName;
    std::filesystem::path source;
    std::string environmentProfile = "ibl_desktop_v1";
    bool optional = false;
};

class SceneCatalog {
  public:
    static constexpr uint32_t kLegacySchemaVersion = 1;
    static constexpr uint32_t kSchemaVersion = 2;

    static SceneCatalog load(const std::filesystem::path &catalogPath,
                             const std::filesystem::path &projectRoot);

    uint32_t schemaVersion = kSchemaVersion;
    std::string projectId;
    std::string defaultImportProfile;
    std::optional<std::string> defaultEnvironment;
    std::vector<CatalogScene> scenes;
    std::vector<CatalogEnvironment> environments;
    std::unordered_map<std::string, ImportProfile> importProfiles;
    std::unordered_map<std::string, EnvironmentProfile> environmentProfiles;

    const ImportProfile &profile(const std::string &id) const;
    const EnvironmentProfile &environmentProfile(const std::string &id) const;
    const CatalogScene *findScene(const std::string &id) const;
    const CatalogEnvironment *findEnvironment(const std::string &id) const;
};

bool isStableAssetId(const std::string &value);
bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate);

} // namespace vkr
