#pragma once

#include <functional>
#include <cstdint>
#include <optional>
#include <string>

namespace vkr {

struct EditorViewportState;

struct EditorFrameStatus {
    std::string sceneName;
    std::string loadingLabel;
    float fps = 0.0f;
    float gpuFrameMs = -1.0f;
    float loadingProgress = 0.0f;
    bool loading = false;
};

struct EditorPanelCallbacks {
    std::function<void()> outliner;
    std::function<void()> inspector;
    std::function<void()> scenes;
    std::function<void()> assets;
    std::function<void()> render;
    std::function<void()> materials;
    std::function<void()> diagnostics;
    std::function<void()> viewportToolbar;
    std::function<void(const EditorViewportState &)> viewportOverlay;

    bool sceneSessionActive = false;
    bool sceneDirty = false;
    bool canUndo = false;
    bool canRedo = false;
    std::string undoLabel;
    std::string redoLabel;
    std::function<void()> newScene;
    std::function<void()> openScene;
    std::function<void()> saveScene;
    std::function<void()> saveSceneAs;
    std::function<void()> closeScene;
    std::function<void()> convertPreview;
    std::function<void()> undo;
    std::function<void()> redo;
};

struct EditorViewportFrame {
    uint64_t textureId = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    bool resizePending = false;
};

enum class EditorWorkspacePreset { Viewport, Debugging, Compact };

struct EditorViewportState {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float logicalWidth = 0.0f;
    float logicalHeight = 0.0f;
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;
    bool valid = false;
    bool visible = false;
    bool hovered = false;
    bool focused = false;
};

class EditorDockWorkspace {
  public:
    void draw(const EditorFrameStatus &status,
              const EditorViewportFrame &viewport,
              const EditorPanelCallbacks &panels);

    const EditorViewportState &viewportState() const {
        return viewportState_;
    }

  private:
    void buildDefaultLayout(unsigned int dockspaceId, float x, float y,
                            float width, float height,
                            EditorWorkspacePreset preset);
    void drawMenuBar(const EditorFrameStatus &status,
                     const EditorPanelCallbacks &panels);
    void drawViewport(const EditorViewportFrame &viewport,
                      const EditorPanelCallbacks &panels);
    void drawPanel(const char *name, bool &visible,
                   const std::function<void()> &callback);

    EditorViewportState viewportState_{};
    bool viewportVisible_ = true;
    bool outlinerVisible_ = true;
    bool inspectorVisible_ = true;
    bool scenesVisible_ = true;
    bool assetsVisible_ = true;
    bool renderVisible_ = true;
    bool materialsVisible_ = true;
    bool diagnosticsVisible_ = false;
    bool resetLayoutRequested_ = false;
    uint8_t activateDefaultTabsFrames_ = 0;
    EditorWorkspacePreset currentPreset_ =
        EditorWorkspacePreset::Viewport;
    std::optional<EditorWorkspacePreset> requestedPreset_;
};

} // namespace vkr
