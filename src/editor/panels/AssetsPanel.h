#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

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
    std::string state;
    uint64_t completed = 0;
    uint64_t total = 0;
    uint64_t encoded = 0;
    uint64_t reused = 0;
    uint64_t failed = 0;
    uint32_t workers = 0;
    double elapsedSeconds = 0.0;
    std::string error;
    std::filesystem::path logPath;
    std::filesystem::path reportPath;
    bool terminal = false;
};

struct AssetsPanelSnapshot {
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

struct AssetsPanelActions {
    std::function<std::optional<EnvironmentImportDefaults>()>
        chooseEnvironment;
    std::function<void(const EnvironmentImportSubmission &)>
        importEnvironment;
    std::function<void(const std::string &, bool)> buildEnvironment;
    std::function<void(const std::string &)> removeEnvironment;
    std::function<void(uint64_t)> cancelTask;
    std::function<void(const std::filesystem::path &)> openPath;
};

class AssetsPanel {
  public:
    void draw(const AssetsPanelSnapshot &snapshot,
              const AssetsPanelActions &actions,
              bool environmentsOnly = false);

  private:
    std::string selectedEnvironmentId_;
    std::optional<EnvironmentImportDefaults> importDraft_;
    std::array<char, 192> importDisplayName_{};
    std::array<char, 128> importEnvironmentId_{};
    int importProfileIndex_ = 0;
    std::string pendingRemoveEnvironmentId_;
};

} // namespace vkr
