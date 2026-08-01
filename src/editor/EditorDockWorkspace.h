#pragma once

#include <functional>
#include <string>

namespace vkr {

struct EditorFrameStatus {
    std::string sceneName;
    std::string loadingLabel;
    float fps = 0.0f;
    float gpuFrameMs = -1.0f;
    float loadingProgress = 0.0f;
    bool loading = false;
};

struct EditorPanelCallbacks {
    std::function<void()> scenes;
    std::function<void()> assets;
    std::function<void()> render;
    std::function<void()> materials;
    std::function<void()> diagnostics;
};

class EditorDockWorkspace {
  public:
    void draw(const EditorFrameStatus &status,
              const EditorPanelCallbacks &panels);

    bool sceneAreaHovered() const;

  private:
    struct SceneArea {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool valid = false;
    };

    void buildDefaultLayout(unsigned int dockspaceId, float x, float y,
                            float width, float height);
    void drawMenuBar(const EditorFrameStatus &status);
    void drawPanel(const char *name, bool &visible,
                   const std::function<void()> &callback);

    SceneArea sceneArea_{};
    bool scenesVisible_ = true;
    bool assetsVisible_ = true;
    bool renderVisible_ = true;
    bool materialsVisible_ = true;
    bool diagnosticsVisible_ = true;
    bool resetLayoutRequested_ = false;
};

} // namespace vkr
