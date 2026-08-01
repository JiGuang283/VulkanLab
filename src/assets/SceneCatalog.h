#pragma once

#include "scene_data/SceneIds.h"
#include "scene/SceneTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

struct SceneDocumentReferences;

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

struct CatalogModel {
    std::string id;
    std::string displayName;
    std::string type = "gltf";
    std::string builtinFactory;
    std::filesystem::path source;
    std::string importProfile;
    bool optional = false;
    std::optional<CameraPose> previewCamera;
};

// Compatibility name for code that still treats a model as its preview scene.
using CatalogScene = CatalogModel;

struct CatalogSceneDocument {
    std::string id;
    std::string displayName;
    std::filesystem::path source;
    bool optional = false;
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
    static constexpr uint32_t kEnvironmentSchemaVersion = 2;
    static constexpr uint32_t kSchemaVersion = 3;

    static SceneCatalog load(const std::filesystem::path &catalogPath,
                             const std::filesystem::path &projectRoot);

    uint32_t schemaVersion = kSchemaVersion;
    std::string projectId;
    std::string defaultImportProfile;
    std::optional<std::string> defaultEnvironment;
    std::vector<CatalogModel> models;
    std::vector<CatalogSceneDocument> sceneDocuments;
    std::vector<CatalogEnvironment> environments;
    std::unordered_map<std::string, ImportProfile> importProfiles;
    std::unordered_map<std::string, EnvironmentProfile> environmentProfiles;

    const ImportProfile &profile(const std::string &id) const;
    const EnvironmentProfile &environmentProfile(const std::string &id) const;
    const CatalogModel *findModel(const std::string &id) const;
    const CatalogSceneDocument *
    findSceneDocument(const std::string &id) const;
    const CatalogModel *findScene(const std::string &id) const {
        return findModel(id);
    }
    const CatalogEnvironment *findEnvironment(const std::string &id) const;

    // Builds reference sets for strict SceneDocument validation.
    SceneDocumentReferences documentReferences() const;
};

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate);

} // namespace vkr
