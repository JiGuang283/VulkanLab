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
    std::string textureEncoder = "uastc";
    std::string qualityPreset = "development";
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

class SceneCatalog {
  public:
    static constexpr uint32_t kSchemaVersion = 1;

    static SceneCatalog load(const std::filesystem::path &catalogPath,
                             const std::filesystem::path &projectRoot);

    uint32_t schemaVersion = kSchemaVersion;
    std::string projectId;
    std::string defaultImportProfile;
    std::vector<CatalogScene> scenes;
    std::unordered_map<std::string, ImportProfile> importProfiles;

    const ImportProfile &profile(const std::string &id) const;
    const CatalogScene *findScene(const std::string &id) const;
};

bool isStableAssetId(const std::string &value);
bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate);

} // namespace vkr
