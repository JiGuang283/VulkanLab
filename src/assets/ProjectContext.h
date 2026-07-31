#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace vkr {

struct ProjectContext {
    std::filesystem::path projectRoot;
    std::filesystem::path runtimeRoot;
    std::filesystem::path catalogPath;
    std::filesystem::path cacheRoot;
    std::filesystem::path captureRoot;
    bool catalogWritable = false;
    bool cookedPackage = false;
    std::string packageProfileId;
    std::string requiredTextureEncoder;
    std::string diagnostic;

    [[nodiscard]] std::filesystem::path resolveProjectPath(
        const std::filesystem::path &path) const;
    [[nodiscard]] std::filesystem::path resolveRuntimePath(
        const std::filesystem::path &path) const;
};

class ProjectContextResolver {
  public:
    static ProjectContext resolve(
        const std::optional<std::filesystem::path> &explicitProjectRoot,
        const std::filesystem::path &executablePath = {});

    static std::filesystem::path currentExecutablePath();
};

} // namespace vkr
