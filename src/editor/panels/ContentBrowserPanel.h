#pragma once

#include "editor/EditorTypes.h"
#include "workflows/SceneWorkflowTypes.h"

#include <array>
#include <functional>
#include <string>

namespace vkr {

struct ContentBrowserSnapshot {
    ContentBrowserViewMode viewMode = ContentBrowserViewMode::Grid;
    const SceneWorkflowSnapshot *scenes = nullptr;
    const AssetWorkflowSnapshot *assets = nullptr;
};

struct ContentBrowserActions {
    std::function<void(ContentBrowserViewMode)> setViewMode;
    std::function<void()> drawScenes;
    std::function<void()> drawModels;
    std::function<void()> drawEnvironments;
    std::function<void(int)> openScene;
    std::function<void(int)> previewModel;
    std::function<void(const std::string &)> assignEnvironment;
};

class ContentBrowserPanel {
  public:
    void draw(const ContentBrowserSnapshot &snapshot,
              const ContentBrowserActions &actions) const;

  private:
    void drawAll(const ContentBrowserSnapshot &snapshot,
                 const ContentBrowserActions &actions) const;

    mutable std::array<char, 128> search_{};
    mutable int category_ = 0;
    mutable int statusFilter_ = 0;
};

} // namespace vkr
