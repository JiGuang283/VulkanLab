#pragma once

#include "PreparedEnvironment.h"

#include <filesystem>
#include <string>

namespace vkr {

class DerivedEnvironmentCache {
  public:
    DerivedEnvironmentCache(std::filesystem::path cacheRoot,
                            std::filesystem::path sourcePath,
                            std::string projectId,
                            std::string environmentId,
                            std::string displayName,
                            std::string profileId,
                            bool validateSource);

    PreparedEnvironmentData load() const;
    const std::filesystem::path &manifestPath() const {
        return manifestPath_;
    }

  private:
    PreparedEnvironmentImage loadImage(
        const DerivedEnvironmentManifest &manifest,
        EnvironmentMapKind kind) const;

    std::filesystem::path cacheRoot_;
    std::filesystem::path sourcePath_;
    std::filesystem::path manifestPath_;
    std::string projectId_;
    std::string environmentId_;
    std::string displayName_;
    std::string profileId_;
    bool validateSource_ = true;
};

} // namespace vkr
