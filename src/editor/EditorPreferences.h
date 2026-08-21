#pragma once

#include "EditorTypes.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace vkr {

struct EditorStoragePaths {
    std::filesystem::path root;
    std::filesystem::path preferences;
    std::filesystem::path layout;

    static EditorStoragePaths resolve(const std::string &projectId);
};

struct EditorPreferences {
    static constexpr int kSchemaVersion = 1;

    EditorWorkspacePreset workspace = EditorWorkspacePreset::Scene;
    ContentBrowserViewMode contentView = ContentBrowserViewMode::Grid;
    GizmoOperation gizmoOperation = GizmoOperation::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;
    float cameraMoveSpeed = 2.0f;
    float bottomDrawerHeight = 240.0f;
    bool renderAdvanced = false;
    bool bottomDrawerExpanded = false;
    bool showBounds = true;
    bool showLights = true;
    bool showProbes = true;
};

class EditorPreferencesStore {
  public:
    explicit EditorPreferencesStore(EditorStoragePaths paths);
    ~EditorPreferencesStore();

    const EditorStoragePaths &paths() const { return paths_; }
    const EditorPreferences &preferences() const { return preferences_; }
    EditorPreferences &edit();

    void update();
    void flush();

  private:
    void load();
    void save();

    EditorStoragePaths paths_;
    EditorPreferences preferences_{};
    std::chrono::steady_clock::time_point changedAt_{};
    bool dirty_ = false;
};

const char *editorWorkspaceName(EditorWorkspacePreset preset);

} // namespace vkr
