#include "ArtifactCachePruner.h"

#include "CacheMutationLock.h"
#include "DerivedTextureManifest.h"
#include "SceneCatalog.h"

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

namespace vkr {
namespace {

struct ScanResult {
    uint64_t manifests = 0;
    uint64_t protectedBlobs = 0;
    uint64_t blobFiles = 0;
    uint64_t blobBytes = 0;
    uint64_t deferredFiles = 0;
    uint64_t deferredBytes = 0;
    std::vector<ArtifactPruneCandidate> candidates;
};

int64_t ageSeconds(const std::filesystem::path &path) {
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    if (writeTime >= now)
        return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(now - writeTime)
        .count();
}

ScanResult scan(const std::filesystem::path &cacheRoot,
                uint32_t olderThanDays) {
    ScanResult result;
    std::set<std::string> protectedPaths;
    const std::filesystem::path manifestRoot = cacheRoot / "manifests";
    std::error_code error;
    if (std::filesystem::is_directory(manifestRoot)) {
        for (std::filesystem::recursive_directory_iterator it(
                 manifestRoot,
                 std::filesystem::directory_options::skip_permission_denied,
                 error),
             end;
             it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".json")
                continue;
            DerivedTextureManifest manifest;
            std::string loadError;
            if (!loadDerivedTextureManifest(it->path(), manifest, loadError)) {
                throw std::runtime_error(
                    "refusing to prune with unreadable manifest '" +
                    it->path().string() + "': " + loadError);
            }
            ++result.manifests;
            for (const DerivedTextureEntry &entry : manifest.entries) {
                const std::filesystem::path blob =
                    (cacheRoot / entry.blob).lexically_normal();
                if (entry.blob.empty() ||
                    !pathIsWithin(cacheRoot / "blobs", blob)) {
                    throw std::runtime_error(
                        "refusing to prune with invalid blob reference in '" +
                        it->path().string() + "'");
                }
                protectedPaths.insert(blob.generic_string());
            }
        }
    }
    result.protectedBlobs = protectedPaths.size();

    const int64_t minimumAge =
        static_cast<int64_t>(olderThanDays) * 24 * 60 * 60;
    const std::filesystem::path blobRoot = cacheRoot / "blobs";
    error.clear();
    if (!std::filesystem::is_directory(blobRoot))
        return result;
    for (std::filesystem::directory_iterator it(blobRoot, error), end;
         it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!it->is_regular_file() || it->path().extension() != ".ktx2")
            continue;
        ++result.blobFiles;
        std::error_code sizeError;
        const uint64_t bytes = it->file_size(sizeError);
        if (!sizeError)
            result.blobBytes += bytes;
        if (protectedPaths.count(it->path().lexically_normal().generic_string()))
            continue;
        const int64_t age = ageSeconds(it->path());
        if (age < minimumAge) {
            ++result.deferredFiles;
            if (!sizeError)
                result.deferredBytes += bytes;
            continue;
        }
        result.candidates.push_back(
            {it->path().lexically_normal(), sizeError ? uint64_t{0} : bytes,
             age});
    }
    return result;
}

void copyScan(const ScanResult &scanResult, ArtifactPruneReport &report) {
    report.manifestFiles = scanResult.manifests;
    report.protectedBlobs = scanResult.protectedBlobs;
    report.scannedBlobFiles = scanResult.blobFiles;
    report.scannedBlobBytes = scanResult.blobBytes;
    report.deferredBlobFiles = scanResult.deferredFiles;
    report.deferredBlobBytes = scanResult.deferredBytes;
    report.candidates = scanResult.candidates;
}

} // namespace

ArtifactPruneReport pruneArtifactCache(const ArtifactPruneOptions &options) {
    if (options.cacheRoot.empty())
        throw std::invalid_argument("cache root is required for prune");
    const std::filesystem::path cacheRoot =
        std::filesystem::absolute(options.cacheRoot).lexically_normal();
    ArtifactPruneReport report;
    if (!options.execute) {
        copyScan(scan(cacheRoot, options.olderThanDays), report);
        return report;
    }

    CacheMutationLock mutationLock(cacheRoot, options.cancelRequested);
    if (options.cancelRequested && options.cancelRequested->load())
        throw std::runtime_error("cache prune cancelled");
    const ScanResult current = scan(cacheRoot, options.olderThanDays);
    copyScan(current, report);
    report.executed = true;
    for (const ArtifactPruneCandidate &candidate : current.candidates) {
        if (options.cancelRequested && options.cancelRequested->load())
            throw std::runtime_error("cache prune cancelled");
        std::error_code removeError;
        if (!std::filesystem::remove(candidate.path, removeError) ||
            removeError) {
            throw std::runtime_error("could not remove cache blob '" +
                                     candidate.path.string() + "': " +
                                     removeError.message());
        }
        ++report.deletedBlobFiles;
        report.deletedBlobBytes += candidate.bytes;
    }
    return report;
}

} // namespace vkr
