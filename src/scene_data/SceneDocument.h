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
enum class ReflectionProbeShape { Box, Sphere };

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
    bool castsShadow = true;
    std::optional<uint32_t> atmosphereSunIndex;
    float sourceAngularRadiusRadians = 0.004675f;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    std::optional<float> range;
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.785398163f;
};

struct AtmosphereComponentDocument {
    float bottomRadiusKm = 6360.0f;
    float atmosphereHeightKm = 100.0f;
    glm::vec3 rayleighScatteringPerKm{0.005802f, 0.013558f, 0.033100f};
    float rayleighScaleHeightKm = 8.0f;
    float mieScatteringPerKm = 0.003996f;
    float mieExtinctionPerKm = 0.004440f;
    float mieScaleHeightKm = 1.2f;
    float mieAnisotropy = 0.8f;
    glm::vec3 ozoneAbsorptionPerKm{0.000650f, 0.001881f, 0.000085f};
    float ozoneCenterHeightKm = 25.0f;
    float ozoneHalfWidthKm = 15.0f;
    glm::vec3 groundAlbedo{0.1f};
    float multipleScatteringFactor = 1.0f;
    float aerialPerspectiveStartMeters = 100.0f;
    float aerialPerspectiveDistanceScale = 1.0f;
};

struct ReflectionProbeComponentDocument {
    std::optional<std::string> environmentId;
    ReflectionProbeShape shape = ReflectionProbeShape::Box;
    glm::vec3 boxExtents{5.0f};
    float sphereRadius = 5.0f;
    float blendDistance = 1.0f;
    int32_t priority = 0;
    float intensity = 1.0f;
    bool boxProjection = true;
    glm::vec3 captureOffset{0.0f};
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
    std::optional<AtmosphereComponentDocument> atmosphere;
    std::optional<ReflectionProbeComponentDocument> reflectionProbe;
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
    static constexpr uint32_t kSchemaVersion = 4;

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
