#pragma once

#include "assets/AssetValidation.h"
#include "assets/SceneImportService.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct SceneWorkflowItemSnapshot {
    int index = -1;
    std::string id;
    std::string displayName;
    std::string sourcePath;
    std::string profileId;
    bool builtin = false;
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

struct ModelImportPanelSubmission {
    std::string displayName;
    std::string modelId;
    std::string profileId;
    bool referenceExisting = false;
    bool loadAfterImport = true;
    bool allowUnvalidated = false;
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
