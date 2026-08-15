#pragma once

#include "workflows/SceneWorkflowTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

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
    void draw(const AssetWorkflowSnapshot &snapshot,
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
