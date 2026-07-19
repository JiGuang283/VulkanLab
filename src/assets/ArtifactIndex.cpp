#include "ArtifactIndex.h"

#include "ContentHash.h"
#include "DerivedTextureManifest.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace vkr {
namespace {

using Json = nlohmann::json;

int64_t unixTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string relativeGeneric(const std::filesystem::path &root,
                            const std::filesystem::path &path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return (error ? path.lexically_normal() : relative.lexically_normal())
        .generic_string();
}

bool hasKtx2Identifier(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ifstream input(path, std::ios::binary);
    std::array<uint8_t, identifier.size()> actual{};
    return input.read(reinterpret_cast<char *>(actual.data()), actual.size()) &&
           actual == identifier;
}

std::wstring indexMutexName(const std::filesystem::path &cacheRoot) {
    std::wstring value =
        std::filesystem::absolute(cacheRoot).lexically_normal().wstring();
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : value) {
        if (character >= L'A' && character <= L'Z')
            character = character - L'A' + L'a';
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ull;
    }
    return L"Local\\VulkanLab.ArtifactIndex." + std::to_wstring(hash);
}

class IndexLock {
  public:
    explicit IndexLock(const std::filesystem::path &cacheRoot) {
        const std::wstring name = indexMutexName(cacheRoot);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_)
            throw std::runtime_error("Could not create ArtifactIndex mutex");
        const DWORD result = WaitForSingleObject(mutex_, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            throw std::runtime_error("Could not lock ArtifactIndex");
        }
    }
    ~IndexLock() {
        if (mutex_) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }

  private:
    HANDLE mutex_ = nullptr;
};

const char *stateName(ArtifactState state) { return artifactStateName(state); }

ArtifactState stateFromName(const std::string &name) {
    if (name == "Ready")
        return ArtifactState::Ready;
    if (name == "Missing")
        return ArtifactState::Missing;
    if (name == "Stale")
        return ArtifactState::Stale;
    if (name == "Importing")
        return ArtifactState::Importing;
    return ArtifactState::Invalid;
}

Json dependencyToJson(const ArtifactIndexDependency &dependency) {
    return {{"path", dependency.path},
            {"size", dependency.size},
            {"writeTime", dependency.writeTime},
            {"sha256", dependency.sha256}};
}

Json blobToJson(const ArtifactIndexBlob &blob) {
    return {{"path", blob.path}, {"bytes", blob.bytes}};
}

Json recordToJson(const ArtifactIndexRecord &record) {
    Json dependencies = Json::array();
    for (const auto &dependency : record.dependencies)
        dependencies.push_back(dependencyToJson(dependency));
    Json blobs = Json::array();
    for (const auto &blob : record.blobs)
        blobs.push_back(blobToJson(blob));
    return {{"sceneId", record.sceneId},
            {"profileId", record.profileId},
            {"textureLimit", record.textureLimit},
            {"state", stateName(record.state)},
            {"reason", record.reason},
            {"manifestPath", record.manifestPath},
            {"manifestSize", record.manifestSize},
            {"manifestWriteTime", record.manifestWriteTime},
            {"manifestSchema", record.manifestSchema},
            {"qualityPreset", record.qualityPreset},
            {"encoderSettings", record.encoderSettings},
            {"dependencies", std::move(dependencies)},
            {"blobs", std::move(blobs)},
            {"lastSuccessfulImportTaskId",
             record.lastSuccessfulImportTaskId},
            {"lastSuccessfulImportUnixMs", record.lastSuccessfulImportUnixMs},
            {"lastAccessUnixMs", record.lastAccessUnixMs},
            {"failureCode", record.failureCode},
            {"failureMessage", record.failureMessage},
            {"failureLogPath", record.failureLogPath},
            {"lastFailureUnixMs", record.lastFailureUnixMs}};
}

ArtifactIndexRecord recordFromJson(const Json &json) {
    ArtifactIndexRecord record;
    record.sceneId = json.at("sceneId").get<std::string>();
    record.profileId = json.at("profileId").get<std::string>();
    record.textureLimit = json.at("textureLimit").get<uint32_t>();
    record.state = stateFromName(json.value("state", "Invalid"));
    record.reason = json.value("reason", std::string{});
    record.manifestPath = json.at("manifestPath").get<std::string>();
    record.manifestSize = json.value("manifestSize", uint64_t{0});
    record.manifestWriteTime = json.value("manifestWriteTime", int64_t{0});
    record.manifestSchema = json.value("manifestSchema", 0u);
    record.qualityPreset = json.value("qualityPreset", std::string{});
    record.encoderSettings = json.value("encoderSettings", std::string{});
    for (const Json &item : json.at("dependencies")) {
        record.dependencies.push_back(
            {item.at("path").get<std::string>(),
             item.at("size").get<uint64_t>(),
             item.at("writeTime").get<int64_t>(),
             item.value("sha256", std::string{})});
    }
    for (const Json &item : json.at("blobs")) {
        record.blobs.push_back({item.at("path").get<std::string>(),
                                item.at("bytes").get<uint64_t>()});
    }
    record.lastSuccessfulImportUnixMs =
        json.value("lastSuccessfulImportUnixMs", int64_t{0});
    record.lastSuccessfulImportTaskId =
        json.value("lastSuccessfulImportTaskId", uint64_t{0});
    record.lastAccessUnixMs = json.value("lastAccessUnixMs", int64_t{0});
    record.failureCode = json.value("failureCode", std::string{});
    record.failureMessage = json.value("failureMessage", std::string{});
    record.failureLogPath = json.value("failureLogPath", std::string{});
    record.lastFailureUnixMs =
        json.value("lastFailureUnixMs", int64_t{0});
    return record;
}

void atomicReplace(const std::filesystem::path &temporary,
                   const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not atomically replace ArtifactIndex "
                                 "(error " +
                                 std::to_string(GetLastError()) + ")");
    }
}

void mergeTelemetry(ArtifactIndexRecord &target,
                    const ArtifactIndexRecord &candidate) {
    if (candidate.lastAccessUnixMs > target.lastAccessUnixMs)
        target.lastAccessUnixMs = candidate.lastAccessUnixMs;
    if (candidate.lastSuccessfulImportUnixMs >
        target.lastSuccessfulImportUnixMs) {
        target.lastSuccessfulImportUnixMs =
            candidate.lastSuccessfulImportUnixMs;
        target.lastSuccessfulImportTaskId =
            candidate.lastSuccessfulImportTaskId;
    }
    if (candidate.lastFailureUnixMs > target.lastFailureUnixMs) {
        target.lastFailureUnixMs = candidate.lastFailureUnixMs;
        target.failureCode = candidate.failureCode;
        target.failureMessage = candidate.failureMessage;
        target.failureLogPath = candidate.failureLogPath;
    }
}

} // namespace

std::string artifactIndexKey(const std::string &sceneId,
                             const std::string &profileId) {
    return sceneId + '\n' + profileId;
}

ArtifactIndex::ArtifactIndex(std::filesystem::path cacheRoot,
                             std::filesystem::path projectRoot,
                             std::string projectId)
    : cacheRoot_(std::filesystem::absolute(std::move(cacheRoot))
                     .lexically_normal()),
      projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      indexPath_(cacheRoot_ / "artifact_index.json"),
      projectId_(std::move(projectId)) {}

ArtifactIndex ArtifactIndex::loadOrRebuild(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &projectRoot, const SceneCatalog &catalog,
    bool *rebuilt, std::string *diagnostic) {
    ArtifactIndex index(cacheRoot, projectRoot, catalog.projectId);
    try {
        std::ifstream input(index.indexPath_, std::ios::binary);
        if (!input)
            throw std::runtime_error("index not found");
        Json root;
        input >> root;
        if (root.value("schemaVersion", 0u) != kSchemaVersion ||
            root.value("projectId", std::string{}) != catalog.projectId)
            throw std::runtime_error("index schema or project mismatch");
        for (const Json &item : root.at("records")) {
            ArtifactIndexRecord record = recordFromJson(item);
            if (!catalog.findScene(record.sceneId) ||
                catalog.importProfiles.count(record.profileId) == 0)
                continue;
            index.records_.emplace(
                artifactIndexKey(record.sceneId, record.profileId),
                std::move(record));
        }
        if (rebuilt)
            *rebuilt = false;
        if (diagnostic)
            *diagnostic = "loaded";
        return index;
    } catch (const std::exception &error) {
        if (diagnostic)
            *diagnostic = error.what();
        index = rebuild(cacheRoot, projectRoot, catalog);
        index.save();
        if (rebuilt)
            *rebuilt = true;
        return index;
    }
}

ArtifactIndex ArtifactIndex::rebuild(const std::filesystem::path &cacheRoot,
                                     const std::filesystem::path &projectRoot,
                                     const SceneCatalog &catalog) {
    ArtifactIndex index(cacheRoot, projectRoot, catalog.projectId);
    index.replaceAllOnSave_ = true;
    for (const CatalogScene &scene : catalog.scenes) {
        if (scene.type != "gltf")
            continue;
        for (const auto &profile : catalog.importProfiles) {
            const auto manifest = derivedManifestPath(
                index.cacheRoot_, scene.id, profile.first);
            if (std::filesystem::is_regular_file(manifest))
                index.refreshRecord(scene, profile.second);
        }
    }
    return index;
}

void ArtifactIndex::refreshRecord(const CatalogScene &scene,
                                  const ImportProfile &profile) {
    const std::string key = artifactIndexKey(scene.id, profile.id);
    const auto existing = records_.find(key);
    const bool sameManifest = existing != records_.end() &&
                              existing->second.manifestSize != 0;
    ArtifactIndexRecord record;
    record.sceneId = scene.id;
    record.profileId = profile.id;
    record.textureLimit = profile.textureLimit;
    const std::filesystem::path scenePath =
        (projectRoot_ / scene.source).lexically_normal();
    const std::filesystem::path manifestPath =
        derivedManifestPath(cacheRoot_, scene.id, profile.id);
    record.manifestPath = relativeGeneric(cacheRoot_, manifestPath);
    const DerivedFileStamp manifestStamp = fileStamp(manifestPath);
    record.manifestSize = manifestStamp.size;
    record.manifestWriteTime = manifestStamp.writeTime;

    const ArtifactStatus inspected = inspectTextureArtifacts(
        {cacheRoot_, scenePath, projectId_, scene.id, profile.id,
         profile.textureLimit});
    record.state = inspected.state;
    record.reason = inspected.reason;
    DerivedTextureManifest manifest;
    std::string loadError;
    if (!loadDerivedTextureManifest(manifestPath, manifest, loadError)) {
        records_[key] = std::move(record);
        dirtyKeys_.insert(key);
        return;
    }
    record.manifestSchema = manifest.schemaVersion;
    record.qualityPreset = manifest.qualityPreset;
    record.encoderSettings = manifest.encoderSettings;
    const auto addDependency = [&](const std::filesystem::path &path,
                                   const DerivedFileStamp &stamp) {
        const std::string relative = relativeGeneric(projectRoot_, path);
        const auto duplicate = std::find_if(
            record.dependencies.begin(), record.dependencies.end(),
            [&](const ArtifactIndexDependency &dependency) {
                return dependency.path == relative;
            });
        if (duplicate == record.dependencies.end()) {
            record.dependencies.push_back(
                {relative, stamp.size, stamp.writeTime, stamp.sha256});
        }
    };
    addDependency(scenePath, manifest.scene);
    const std::filesystem::path sceneDirectory = scenePath.parent_path();
    std::set<std::string> blobPaths;
    for (const DerivedTextureEntry &entry : manifest.entries) {
        std::filesystem::path source(entry.source.path);
        if (!source.is_absolute())
            source = sceneDirectory / source;
        source = source.lexically_normal();
        if (source != scenePath)
            addDependency(source, entry.source);
        if (blobPaths.insert(entry.blob).second) {
            std::error_code sizeError;
            const uint64_t bytes =
                std::filesystem::file_size(cacheRoot_ / entry.blob, sizeError);
            record.blobs.push_back(
                {entry.blob, sizeError ? uint64_t{0} : bytes});
        }
    }
    std::sort(record.dependencies.begin(), record.dependencies.end(),
              [](const auto &left, const auto &right) {
                  return left.path < right.path;
              });
    std::sort(record.blobs.begin(), record.blobs.end(),
              [](const auto &left, const auto &right) {
                  return left.path < right.path;
              });
    if (record.state == ArtifactState::Ready) {
        record.lastSuccessfulImportUnixMs =
            sameManifest && existing->second.manifestSize == record.manifestSize &&
                    existing->second.manifestWriteTime == record.manifestWriteTime
                ? existing->second.lastSuccessfulImportUnixMs
                : unixTimeMs();
    }
    if (existing != records_.end()) {
        record.lastAccessUnixMs = existing->second.lastAccessUnixMs;
        record.lastSuccessfulImportTaskId =
            existing->second.lastSuccessfulImportTaskId;
        record.failureCode = existing->second.failureCode;
        record.failureMessage = existing->second.failureMessage;
        record.failureLogPath = existing->second.failureLogPath;
        record.lastFailureUnixMs = existing->second.lastFailureUnixMs;
    }
    records_[key] = std::move(record);
    dirtyKeys_.insert(key);
}

void ArtifactIndex::refresh(const SceneCatalog &catalog,
                            const std::string &sceneId,
                            const std::string &profileId) {
    const CatalogScene *scene = catalog.findScene(sceneId);
    if (!scene || scene->type != "gltf")
        throw std::runtime_error("Cannot index unknown glTF scene: " + sceneId);
    refreshRecord(*scene, catalog.profile(profileId));
}

ArtifactStatus ArtifactIndex::query(const ArtifactStatusRequest &request,
                                    ArtifactValidationMode mode) {
    const std::filesystem::path manifestPath = derivedManifestPath(
        cacheRoot_, request.sceneId, request.profileId);
    const auto found = records_.find(
        artifactIndexKey(request.sceneId, request.profileId));
    if (found == records_.end())
        return {ArtifactState::Missing, "artifact index has no record",
                manifestPath};
    ArtifactIndexRecord &record = found->second;
    ArtifactStatus status{record.state, record.reason, manifestPath,
                          record.blobs.size(), 0};
    for (const auto &blob : record.blobs)
        status.blobBytes += blob.bytes;
    if (record.state != ArtifactState::Ready)
        return status;
    if (record.textureLimit != request.textureLimit ||
        request.projectId != projectId_)
        return {ArtifactState::Invalid, "index identity or profile mismatch",
                manifestPath};

    const DerivedFileStamp currentManifest = fileStamp(manifestPath);
    if (currentManifest.size != record.manifestSize ||
        currentManifest.writeTime != record.manifestWriteTime) {
        return {ArtifactState::Invalid, "manifest changed after indexing",
                manifestPath};
    }

    bool stampsUpdated = false;
    for (ArtifactIndexDependency &dependency : record.dependencies) {
        const std::filesystem::path path =
            (projectRoot_ / dependency.path).lexically_normal();
        if (!std::filesystem::is_regular_file(path))
            return {ArtifactState::Stale,
                    "source dependency is missing: " + dependency.path,
                    manifestPath};
        const DerivedFileStamp current = fileStamp(path);
        if (current.size == dependency.size &&
            current.writeTime == dependency.writeTime)
            continue;
        if (dependency.sha256.empty() ||
            sha256File(path) != dependency.sha256) {
            return {ArtifactState::Stale,
                    "source dependency changed: " + dependency.path,
                    manifestPath};
        }
        dependency.size = current.size;
        dependency.writeTime = current.writeTime;
        stampsUpdated = true;
    }
    if (mode == ArtifactValidationMode::Admission) {
        for (const ArtifactIndexBlob &blob : record.blobs) {
            const std::filesystem::path path =
                (cacheRoot_ / blob.path).lexically_normal();
            std::error_code sizeError;
            const uint64_t bytes = std::filesystem::file_size(path, sizeError);
            if (sizeError || bytes != blob.bytes || !hasKtx2Identifier(path)) {
                return {ArtifactState::Invalid,
                        "derived blob is missing or invalid: " + blob.path,
                        manifestPath};
            }
        }
    }
    if (stampsUpdated)
        dirtyKeys_.insert(artifactIndexKey(request.sceneId,
                                           request.profileId));
    if (stampsUpdated) {
        try {
            save();
        } catch (const std::exception &) {
            // The validated in-memory record remains usable. A later refresh
            // or access update can persist the corrected file stamp.
        }
    }
    status.state = ArtifactState::Ready;
    status.reason = "ready (artifact index)";
    return status;
}

void ArtifactIndex::recordFailure(const std::string &sceneId,
                                  const std::string &profileId,
                                  const std::string &code,
                                  const std::string &message,
                                  const std::filesystem::path &logPath) {
    ArtifactIndexRecord &record =
        records_[artifactIndexKey(sceneId, profileId)];
    record.sceneId = sceneId;
    record.profileId = profileId;
    record.failureCode = code;
    record.failureMessage = message;
    record.failureLogPath = logPath.string();
    record.lastFailureUnixMs = unixTimeMs();
    dirtyKeys_.insert(artifactIndexKey(sceneId, profileId));
}

void ArtifactIndex::recordImportSuccess(const std::string &sceneId,
                                        const std::string &profileId,
                                        uint64_t taskId) {
    const std::string key = artifactIndexKey(sceneId, profileId);
    const auto found = records_.find(key);
    if (found == records_.end())
        return;
    found->second.lastSuccessfulImportTaskId = taskId;
    found->second.lastSuccessfulImportUnixMs = unixTimeMs();
    dirtyKeys_.insert(key);
}

void ArtifactIndex::touch(const std::string &sceneId,
                          const std::string &profileId) {
    const auto found = records_.find(artifactIndexKey(sceneId, profileId));
    if (found != records_.end()) {
        found->second.lastAccessUnixMs = unixTimeMs();
        dirtyKeys_.insert(artifactIndexKey(sceneId, profileId));
    }
}

ArtifactIndexUsage ArtifactIndex::usage() const {
    ArtifactIndexUsage result;
    result.records = records_.size();
    std::set<std::string> referenced;
    for (const auto &pair : records_) {
        const ArtifactIndexRecord &record = pair.second;
        if (record.state == ArtifactState::Ready)
            ++result.readyRecords;
        for (const ArtifactIndexBlob &blob : record.blobs) {
            if (referenced.insert(blob.path).second) {
                ++result.referencedBlobs;
                result.referencedBlobBytes += blob.bytes;
            }
        }
    }
    const std::filesystem::path blobRoot = cacheRoot_ / "blobs";
    std::error_code iteratorError;
    if (std::filesystem::is_directory(blobRoot)) {
        for (std::filesystem::directory_iterator it(blobRoot, iteratorError),
             end;
             it != end; it.increment(iteratorError)) {
            if (iteratorError) {
                iteratorError.clear();
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".ktx2")
                continue;
            ++result.cacheBlobFiles;
            std::error_code sizeError;
            const uint64_t bytes = it->file_size(sizeError);
            if (!sizeError)
                result.cacheBlobBytes += bytes;
            const std::string relative = relativeGeneric(cacheRoot_, it->path());
            if (referenced.count(relative) == 0) {
                ++result.unreferencedBlobFiles;
                if (!sizeError)
                    result.unreferencedBlobBytes += bytes;
            }
        }
    }
    return result;
}

void ArtifactIndex::save() {
    if (!replaceAllOnSave_ && dirtyKeys_.empty() &&
        std::filesystem::is_regular_file(indexPath_))
        return;
    IndexLock lock(cacheRoot_);
    std::filesystem::create_directories(indexPath_.parent_path());
    std::unordered_map<std::string, ArtifactIndexRecord> merged;
    if (!replaceAllOnSave_) {
        try {
            std::ifstream input(indexPath_, std::ios::binary);
            Json existingRoot;
            input >> existingRoot;
            if (existingRoot.value("schemaVersion", 0u) == kSchemaVersion &&
                existingRoot.value("projectId", std::string{}) == projectId_) {
                for (const Json &item : existingRoot.at("records")) {
                    ArtifactIndexRecord record = recordFromJson(item);
                    merged[artifactIndexKey(record.sceneId,
                                            record.profileId)] =
                        std::move(record);
                }
            }
        } catch (const std::exception &) {
            merged.clear();
        }
        for (const std::string &key : dirtyKeys_) {
            const auto found = records_.find(key);
            if (found == records_.end())
                continue;
            const auto disk = merged.find(key);
            if (disk == merged.end()) {
                merged[key] = found->second;
                continue;
            }
            const std::filesystem::path manifest =
                derivedManifestPath(cacheRoot_, found->second.sceneId,
                                    found->second.profileId);
            const DerivedFileStamp current = fileStamp(manifest);
            const bool localIsCurrent =
                found->second.manifestSize == current.size &&
                found->second.manifestWriteTime == current.writeTime;
            const bool diskIsCurrent = disk->second.manifestSize == current.size &&
                                       disk->second.manifestWriteTime ==
                                           current.writeTime;
            ArtifactIndexRecord selected =
                diskIsCurrent && !localIsCurrent ? disk->second
                                                 : found->second;
            mergeTelemetry(selected, disk->second);
            mergeTelemetry(selected, found->second);
            disk->second = std::move(selected);
        }
    } else {
        merged = records_;
    }
    Json records = Json::array();
    std::vector<const ArtifactIndexRecord *> ordered;
    ordered.reserve(merged.size());
    for (const auto &pair : merged)
        ordered.push_back(&pair.second);
    std::sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                 const auto *right) {
        return artifactIndexKey(left->sceneId, left->profileId) <
               artifactIndexKey(right->sceneId, right->profileId);
    });
    for (const auto *record : ordered)
        records.push_back(recordToJson(*record));
    const Json root = {{"schemaVersion", kSchemaVersion},
                       {"projectId", projectId_},
                       {"updatedUnixMs", unixTimeMs()},
                       {"records", std::move(records)}};
    const std::filesystem::path temporary =
        indexPath_.string() + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not create ArtifactIndex temp file");
        output << root.dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not flush ArtifactIndex temp file");
        output.close();
        atomicReplace(temporary, indexPath_);
        records_ = std::move(merged);
        dirtyKeys_.clear();
        replaceAllOnSave_ = false;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace vkr
