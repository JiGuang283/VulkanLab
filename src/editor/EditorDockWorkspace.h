#pragma once

#include <functional>
#include <cstdint>
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

struct EditorViewportFrame {
    uint64_t textureId = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
};

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
                            float width, float height);
    void drawMenuBar(const EditorFrameStatus &status);
    void drawViewport(const EditorViewportFrame &viewport);
    void drawPanel(const char *name, bool &visible,
                   const std::function<void()> &callback);

    EditorViewportState viewportState_{};
    bool viewportVisible_ = true;
    bool scenesVisible_ = true;
    bool assetsVisible_ = true;
    bool renderVisible_ = true;
    bool materialsVisible_ = true;
    bool diagnosticsVisible_ = true;
    bool resetLayoutRequested_ = false;
};

} // namespace vkr
