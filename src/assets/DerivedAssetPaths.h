#pragma once

#include <filesystem>
#include <string>

namespace vkr {

class DerivedAssetPaths {
  public:
    static std::filesystem::path defaultCacheRoot(const std::string &projectId);

    DerivedAssetPaths(std::filesystem::path cacheRoot, std::string sceneId,
                      std::string profileId);

    const std::filesystem::path &cacheRoot() const { return cacheRoot_; }
    std::filesystem::path manifestPath() const;
    std::filesystem::path blobPath(const std::string &cacheKey) const;

  private:
    std::filesystem::path cacheRoot_;
    std::string sceneId_;
    std::string profileId_;
};

} // namespace vkr
