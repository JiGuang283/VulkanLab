#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace vkr {

struct ProjectContext {
    std::filesystem::path projectRoot;
    std::filesystem::path catalogPath;
    std::filesystem::path cacheRoot;
    bool catalogWritable = false;
    std::string diagnostic;
};

class ProjectContextResolver {
  public:
    static ProjectContext resolve(
        const std::optional<std::filesystem::path> &explicitProjectRoot,
        const std::filesystem::path &executablePath = {});

    static std::filesystem::path currentExecutablePath();
};

} // namespace vkr
