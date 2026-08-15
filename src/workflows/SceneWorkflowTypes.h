#pragma once

#include "assets/AssetValidation.h"
#include "assets/ModelImportService.h"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class SceneWorkflowRequestReason {
    SceneLoad,
    SceneRegistration,
    ManualReimport
};

struct SceneWorkflowItemSnapshot {
    int index = -1;
    std::string id;
    std::string displayName;
    std::string sourcePath;
    std::string profileId;
    bool available = true;
    std::string unavailableReason;
    std::string artifactState = "Unknown";
    std::string artifactReason;
    std::string encoder;
    std::string validationState;
    std::string validationReason;
    std::filesystem::path validationReportPath;
    bool current = false;
    bool canAuthor = false;
    bool canLoadSource = false;
    bool canEditCatalog = false;
    bool canInstantiate = false;
};

struct EnginePrimitiveItemSnapshot {
    std::string id;
    std::string displayName;
    bool canInstantiate = false;
};

struct SceneWorkflowSnapshot {
    std::string projectId;
    std::vector<SceneWorkflowItemSnapshot> models;
    std::vector<EnginePrimitiveItemSnapshot> enginePrimitives;
    std::vector<SceneWorkflowItemSnapshot> nativeScenes;
    int selectedIndex = -1;
    bool showImport = false;
    bool canImport = false;
    bool busy = false;
    std::string status;
    std::string error;
    bool copyActive = false;
    float copyProgress = 0.0f;
    std::string copyFile;
    bool openImportDialog = false;
    std::optional<ModelImportPreflight> importPreflight;
    std::optional<AssetValidationReport> importValidation;
    std::filesystem::path importValidationReportPath;
    std::vector<std::string> importProfileIds;
    int defaultImportProfileIndex = 0;
    bool defaultReferenceExisting = false;
    bool defaultLoadAfterImport = true;
};

struct SceneWorkflowRuntimeState {
    uint32_t textureLimit = 0;
    int currentSceneIndex = -1;
    bool nativeSceneSessionActive = false;
};

struct AssetArtifactSnapshot {
    std::string modelName;
    std::string profileId;
    std::string encoder;
    std::string state;
    std::string reason;
    std::string payloadKind;
    uint64_t entryCount = 0;
    uint64_t blobBytes = 0;
    std::string failureCode;
    std::string failureMessage;
};

struct EnvironmentAssetSnapshot {
    std::string id;
    std::string displayName;
    std::string source;
    std::string profileId;
    std::string artifactState;
    std::string artifactReason;
    uint64_t entryCount = 0;
    uint64_t blobBytes = 0;
    bool ready = false;
};

struct AssetTaskSnapshot {
    uint64_t id = 0;
    std::string kind;
    std::string assetId;
    std::string profileId;
    std::filesystem::path sourcePath;
    std::string state;
    std::string phase;
    uint64_t completed = 0;
    uint64_t total = 0;
    uint64_t encoded = 0;
    uint64_t reused = 0;
    uint64_t failed = 0;
    uint32_t workers = 0;
    uint64_t activeImage = 0;
    uint64_t estimatedMemoryBytes = 0;
    double elapsedSeconds = 0.0;
    std::string error;
    std::filesystem::path logPath;
    std::filesystem::path manifestPath;
    std::filesystem::path reportPath;
    int processExitCode = 0;
    bool terminal = false;
    std::string validationState;
    uint64_t validationErrors = 0;
    uint64_t validationWarnings = 0;
    std::string validationReportKey;
    std::string validationInputFingerprint;
    std::string validationFailureReason;
    uint64_t linkedSceneLoadTaskId = 0;
};

struct AssetWorkflowSnapshot {
    std::string projectId;
    std::string mode;
    std::string catalogPath;
    std::string cachePath;
    bool hasUsage = false;
    uint64_t indexRecords = 0;
    uint64_t readyRecords = 0;
    uint64_t cacheBlobFiles = 0;
    uint64_t cacheBlobBytes = 0;
    uint64_t unreferencedBlobFiles = 0;
    uint64_t unreferencedBlobBytes = 0;
    uint64_t referencedBlobs = 0;
    uint64_t referencedBlobBytes = 0;
    uint32_t indexSchema = 0;
    std::optional<AssetArtifactSnapshot> selectedModel;
    std::vector<EnvironmentAssetSnapshot> environments;
    bool canEditEnvironments = false;
    bool canBuildEnvironments = false;
    std::string environmentStatus;
    std::string environmentError;
    std::optional<AssetTaskSnapshot> activeTask;
    std::vector<AssetTaskSnapshot> recentTasks;
    bool authoringCompiled = false;
};

struct ModelImportPanelSubmission {
    std::string displayName;
    std::string modelId;
    std::string profileId;
    bool referenceExisting = false;
    bool loadAfterImport = true;
    bool allowUnvalidated = false;
};

struct EnvironmentImportDefaults {
    std::filesystem::path source;
    std::string displayName;
    std::string environmentId;
    std::vector<std::string> profileIds;
};

struct EnvironmentImportSubmission {
    std::filesystem::path source;
    std::string displayName;
    std::string environmentId;
    std::string profileId;
};

struct SceneWorkflowActions {
    std::function<void()> beginModelImport;
    std::function<void(int)> selectModel;
    std::function<void(int)> loadPreview;
    std::function<void(int)> loadSceneDocument;
    std::function<void(int)> reimportModel;
    std::function<void(int)> validateModel;
    std::function<void(int)> loadSourceFallback;
    std::function<void(int)> savePreviewCamera;
    std::function<void(int)> removeModel;
    std::function<void(const std::filesystem::path &)> openReport;
    std::function<void()> cancelImport;
    std::function<void(const ModelImportPanelSubmission &)> confirmImport;
    std::function<void()> dismissImport;
    std::function<void()> refresh;
};

} // namespace vkr
