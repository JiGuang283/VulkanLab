#pragma once

#include "DerivedTextureManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class AssetValidationState {
    NotChecked,
    Valid,
    Warnings,
    Invalid,
    Stale,
    Unavailable,
    Failed,
    NotApplicable
};

const char *assetValidationStateName(AssetValidationState state);
std::optional<AssetValidationState>
assetValidationStateFromName(const std::string &name);

enum class GltfExtensionSupport { Supported, Partial, Unsupported };

const char *gltfExtensionSupportName(GltfExtensionSupport support);
GltfExtensionSupport gltfExtensionSupport(const std::string &extension);

struct AssetValidationIssue {
    std::string code;
    std::string message;
    std::string pointer;
    uint32_t severity = 0;
};

struct GltfExtensionDiagnostic {
    std::string name;
    GltfExtensionSupport support = GltfExtensionSupport::Unsupported;
    bool required = false;
    std::string note;
};

struct AssetValidationStatistics {
    std::string assetVersion;
    std::string generator;
    uint64_t animationCount = 0;
    uint64_t materialCount = 0;
    uint64_t drawCallCount = 0;
    uint64_t totalVertexCount = 0;
    uint64_t totalTriangleCount = 0;
    uint64_t maxUvs = 0;
    uint64_t maxInfluences = 0;
    uint64_t maxAttributes = 0;
    bool hasMorphTargets = false;
    bool hasSkins = false;
    bool hasTextures = false;
    bool hasDefaultScene = false;
};

struct AssetValidationReport {
    static constexpr uint32_t kSchemaVersion = 1;

    uint32_t schemaVersion = kSchemaVersion;
    AssetValidationState state = AssetValidationState::NotChecked;
    std::string validatorName = "Khronos glTF Validator";
    std::string validatorVersion;
    std::string reportKey;
    std::string inputFingerprint;
    std::string sourceSha256;
    DerivedFileStamp source;
    std::vector<DerivedFileStamp> dependencies;
    uint64_t errorCount = 0;
    uint64_t warningCount = 0;
    uint64_t infoCount = 0;
    uint64_t hintCount = 0;
    bool truncated = false;
    std::vector<AssetValidationIssue> issues;
    std::vector<GltfExtensionDiagnostic> extensions;
    AssetValidationStatistics statistics;
    std::string failureReason;
    int64_t generatedUnixMs = 0;
};

struct SceneValidationReceipt {
    std::string reportKey;
    std::string inputFingerprint;
    AssetValidationState state = AssetValidationState::NotChecked;
};

struct AssetValidationQuery {
    AssetValidationState state = AssetValidationState::NotChecked;
    std::string reason;
    std::filesystem::path reportPath;
    std::optional<AssetValidationReport> report;
};

std::string assetValidationInputFingerprint(
    const std::string &sourceSha256, const DerivedFileStamp &source,
    const std::vector<DerivedFileStamp> &dependencies);

std::filesystem::path assetValidationReportPath(
    const std::filesystem::path &cacheRoot,
    const std::string &validatorVersion,
    const std::string &inputFingerprint);

bool saveAssetValidationReport(const std::filesystem::path &cacheRoot,
                               AssetValidationReport &report,
                               std::string &error);
bool loadAssetValidationReport(const std::filesystem::path &path,
                               AssetValidationReport &report,
                               std::string &error);

SceneValidationReceipt
sceneValidationReceipt(const AssetValidationReport &report);

bool validationReportMatchesSource(const AssetValidationReport &report,
                                   const std::filesystem::path &sourcePath,
                                   std::string &reason);

SceneValidationReceipt bindSceneValidation(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &projectRoot, const std::string &sceneId,
    const std::filesystem::path &projectRelativeSource,
    const SceneValidationReceipt &receipt);

AssetValidationQuery querySceneValidation(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &projectRoot, const std::string &sceneId);

void removeSceneValidationBinding(
    const std::filesystem::path &cacheRoot, const std::string &sceneId);

std::vector<std::filesystem::path> referencedAssetValidationReports(
    const std::filesystem::path &cacheRoot);

AssetValidationQuery queryValidationReceipt(
    const std::filesystem::path &cacheRoot,
    const SceneValidationReceipt &receipt,
    const std::filesystem::path &sourcePath);

} // namespace vkr
