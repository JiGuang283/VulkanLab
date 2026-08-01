#pragma once

#include "SceneIds.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace vkr {

enum class SceneDocumentLightType { Directional, Point, Spot };

struct SceneTransformDocument {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct ModelInstanceDocument {
    ModelAssetId model;
};

struct LightComponentDocument {
    SceneDocumentLightType type = SceneDocumentLightType::Directional;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    std::optional<float> range;
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.785398163f;
};

struct CameraComponentDocument {
    float verticalFovRadians = 1.04719755f;
    float nearPlane = 0.05f;
    float farPlane = 1000.0f;
};

struct SceneEntityDocument {
    PersistentEntityId id;
    std::string name;
    std::optional<PersistentEntityId> parent;
    bool enabled = true;
    SceneTransformDocument transform;
    std::optional<ModelInstanceDocument> modelInstance;
    std::optional<LightComponentDocument> light;
    std::optional<CameraComponentDocument> camera;
};

struct SceneAmbientDocument {
    glm::vec3 color{0.03f};
    float intensity = 1.0f;
};

struct SceneEnvironmentDocument {
    std::string environmentId;
    float intensity = 1.0f;
    float rotationRadians = 0.0f;
};

struct SceneDocument {
    static constexpr uint32_t kSchemaVersion = 1;

    uint32_t schemaVersion = kSchemaVersion;
    SceneDocumentId id;
    std::string displayName;
    std::optional<PersistentEntityId> activeCamera;
    SceneAmbientDocument ambient;
    std::optional<SceneEnvironmentDocument> environment;
    std::vector<SceneEntityDocument> entities;
};

struct SceneDocumentReferences {
    std::unordered_set<std::string> modelIds;
    std::unordered_set<std::string> environmentIds;
};

struct SceneDocumentFileStamp {
    uint64_t size = 0;
    int64_t writeTime = 0;
    bool exists = false;

    friend bool operator==(const SceneDocumentFileStamp &left,
                           const SceneDocumentFileStamp &right) {
        return left.size == right.size && left.writeTime == right.writeTime &&
               left.exists == right.exists;
    }
    friend bool operator!=(const SceneDocumentFileStamp &left,
                           const SceneDocumentFileStamp &right) {
        return !(left == right);
    }
};

struct LoadedSceneDocument {
    SceneDocument document;
    std::filesystem::path path;
    SceneDocumentFileStamp sourceStamp;
};

class SceneDocumentService {
  public:
    static LoadedSceneDocument
    load(const std::filesystem::path &path,
         const std::filesystem::path &projectRoot,
         const SceneDocumentReferences *references = nullptr);

    static SceneDocument createDefault(const std::string &id,
                                       const std::string &displayName);

    static void validate(const SceneDocument &document,
                         const SceneDocumentReferences *references = nullptr);

    static SceneDocumentFileStamp
    saveAtomic(const std::filesystem::path &path,
               const std::filesystem::path &projectRoot,
               const SceneDocument &document,
               std::optional<SceneDocumentFileStamp> expectedStamp =
                   std::nullopt);

    static SceneDocumentFileStamp
    fileStamp(const std::filesystem::path &path);
};

} // namespace vkr
