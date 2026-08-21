#pragma once

#include "editor/EditorTypes.h"
#include "workflows/SceneWorkflowTypes.h"

#include <array>
#include <string>

namespace vkr {

class ScenesPanel {
  public:
    void draw(const SceneWorkflowSnapshot &snapshot,
              const SceneWorkflowActions &actions,
              bool modelsOnly = false,
              ContentBrowserViewMode viewMode =
                  ContentBrowserViewMode::List);

  private:
    void drawImportDialog(const SceneWorkflowSnapshot &snapshot,
                          const SceneWorkflowActions &actions);

    std::array<char, 128> search_{};
    std::array<char, 192> importDisplayName_{};
    std::array<char, 128> importModelId_{};
    int importProfileIndex_ = 0;
    bool importReferenceExisting_ = false;
    bool importLoadAfter_ = true;
    bool importAllowUnvalidated_ = false;
    int pendingRemoveIndex_ = -1;
};

} // namespace vkr
