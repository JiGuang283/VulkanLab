#pragma once

#include "ArtifactStatus.h"
#include "SceneCatalog.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vkr {

enum class ArtifactValidationMode { Fast, Admission };
enum class ArtifactKind { Scene, Environment };

const char *artifactKindName(ArtifactKind kind);

struct ArtifactIndexDependency {
    std::string path;
    uint64_t size = 0;
    int64_t writeTime = 0;
    std::string sha256;
};

struct ArtifactIndexBlob {
    std::string path;
    uint64_t bytes = 0;
};

struct ArtifactIndexRecord {
    ArtifactKind assetKind = ArtifactKind::Scene;
    std::string assetId;
    std::string sceneId;
    std::string profileId;
    uint32_t textureLimit = 0;
    ArtifactState state = ArtifactState::Invalid;
    std::string reason;
    std::string manifestPath;
    uint64_t manifestSize = 0;
    int64_t manifestWriteTime = 0;
    uint32_t manifestSchema = 0;
    std::string qualityPreset;
    std::string textureEncoder;
    std::string encoderSettings;
    std::vector<ArtifactIndexDependency> dependencies;
    std::vector<ArtifactIndexBlob> blobs;
    uint64_t lastSuccessfulImportTaskId = 0;
    int64_t lastSuccessfulImportUnixMs = 0;
    int64_t lastAccessUnixMs = 0;
    std::string failureCode;
    std::string failureMessage;
    std::string failureLogPath;
    int64_t lastFailureUnixMs = 0;
};

struct ArtifactIndexUsage {
    uint64_t records = 0;
    uint64_t readyRecords = 0;
    uint64_t referencedBlobs = 0;
    uint64_t referencedBlobBytes = 0;
    uint64_t cacheBlobFiles = 0;
    uint64_t cacheBlobBytes = 0;
    uint64_t unreferencedBlobFiles = 0;
    uint64_t unreferencedBlobBytes = 0;
};

class ArtifactIndex {
  public:
    static constexpr uint32_t kLegacySchemaVersion = 1;
    static constexpr uint32_t kSchemaVersion = 3;

    static ArtifactIndex loadOrRebuild(
        const std::filesystem::path &cacheRoot,
        const std::filesystem::path &projectRoot,
        const SceneCatalog &catalog, bool *rebuilt = nullptr,
        std::string *diagnostic = nullptr);
    static ArtifactIndex rebuild(const std::filesystem::path &cacheRoot,
                                 const std::filesystem::path &projectRoot,
                                 const SceneCatalog &catalog);

    ArtifactStatus query(const ArtifactStatusRequest &request,
                         ArtifactValidationMode mode);
    void refresh(const SceneCatalog &catalog, const std::string &sceneId,
                 const std::string &profileId);
    void refreshEnvironment(const SceneCatalog &catalog,
                            const std::string &environmentId,
                            const std::string &profileId);
    void recordFailure(const std::string &sceneId,
                       const std::string &profileId,
                       const std::string &code,
                       const std::string &message,
                       const std::filesystem::path &logPath);
    void recordImportSuccess(const std::string &sceneId,
                             const std::string &profileId, uint64_t taskId);
    void touch(const std::string &sceneId, const std::string &profileId);
    void touchEnvironment(const std::string &environmentId,
                          const std::string &profileId);
    ArtifactIndexUsage usage() const;
    void save();

    const std::unordered_map<std::string, ArtifactIndexRecord> &records() const {
        return records_;
    }
    const std::filesystem::path &path() const { return indexPath_; }

  private:
    ArtifactIndex(std::filesystem::path cacheRoot,
                  std::filesystem::path projectRoot, std::string projectId);
    void refreshRecord(const CatalogModel &scene,
                       const ImportProfile &profile);
    void refreshEnvironmentRecord(
        const CatalogEnvironment &environment,
        const EnvironmentProfile &profile);

    std::filesystem::path cacheRoot_;
    std::filesystem::path projectRoot_;
    std::filesystem::path indexPath_;
    std::string projectId_;
    std::unordered_map<std::string, ArtifactIndexRecord> records_;
    std::unordered_set<std::string> dirtyKeys_;
    bool replaceAllOnSave_ = false;
};

std::string artifactIndexKey(const std::string &sceneId,
                             const std::string &profileId);
std::string artifactIndexKey(ArtifactKind kind, const std::string &assetId,
                             const std::string &profileId);

} // namespace vkr
