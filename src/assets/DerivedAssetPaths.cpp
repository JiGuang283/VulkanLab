#include "DerivedAssetPaths.h"

#include <cstdlib>
#include <stdexcept>

namespace vkr {

std::filesystem::path
DerivedAssetPaths::defaultCacheRoot(const std::string &projectId) {
    if (projectId.empty())
        throw std::invalid_argument("projectId cannot be empty");
#ifdef _WIN32
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData)
        return std::filesystem::path(localAppData) / "VulkanLab" /
               "DerivedAssets" / projectId;
#endif
    return std::filesystem::temp_directory_path() / "VulkanLab" /
           "DerivedAssets" / projectId;
}

DerivedAssetPaths::DerivedAssetPaths(std::filesystem::path cacheRoot,
                                     std::string sceneId,
                                     std::string profileId)
    : cacheRoot_(std::move(cacheRoot)), sceneId_(std::move(sceneId)),
      profileId_(std::move(profileId)) {
    if (sceneId_.empty() || profileId_.empty())
        throw std::invalid_argument("sceneId and profileId cannot be empty");
}

std::filesystem::path DerivedAssetPaths::manifestPath() const {
    return cacheRoot_ / "manifests" / sceneId_ / (profileId_ + ".json");
}

std::filesystem::path
DerivedAssetPaths::blobPath(const std::string &cacheKey) const {
    return cacheRoot_ / "blobs" / (cacheKey + ".ktx2");
}

} // namespace vkr
