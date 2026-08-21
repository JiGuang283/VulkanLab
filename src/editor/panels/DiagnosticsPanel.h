#pragma once

#include "diagnostics/SceneLoadStats.h"
#include "render/Renderer.h"
#include "render/diagnostics/GpuPassProfiler.h"
#include "render/graph/RenderGraph.h"
#include "render/material/MaterialSystem.h"
#include "render/path/RenderPath.h"
#include "scene/AssetRepository.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>

namespace vkr {

struct DiagnosticsPanelSnapshot {
    bool sceneLoadActive = false;
    bool captureAvailable = false;
    float fps = 0.0f;
    float gpuFrameMs = 0.0f;
    bool cameraDragging = false;
    size_t renderableCount = 0;
    bool tracyCompiled = false;
    bool tracyConnected = false;
    bool tracyGpuAvailable = false;
    GpuPassTimings gpuTimings;
    RenderPathStatus renderPath;
    GBufferRuntimeStatus gBuffer;
    DeferredLightingRuntimeStatus deferredLighting;
    ClusteredLightingStatus clusteredLighting;
    RenderGraphDiagnostics renderGraph;
    const SceneLoadStats *lastSceneLoad = nullptr;
    MaterialBindingStatus materialBinding;
    AssetRepositorySnapshot modelRepository;
};

struct DiagnosticsPanelActions {
    std::function<void()> drawSceneLoad;
    std::function<void()> drawTasks;
    std::function<void()> drawCapture;
    std::function<void()> exportRenderGraphJson;
    std::function<void()> exportRenderGraphDot;
};

class DiagnosticsPanel {
  public:
    void draw(const DiagnosticsPanelSnapshot &snapshot,
              const DiagnosticsPanelActions &actions);

  private:
    void drawPerformance(const DiagnosticsPanelSnapshot &snapshot,
                         const DiagnosticsPanelActions &actions);
    void drawLoadStats(const DiagnosticsPanelSnapshot &snapshot);

    static constexpr size_t kHistorySize = 180;
    std::array<float, kHistorySize> fpsHistory_{};
    std::array<float, kHistorySize> gpuHistory_{};
    size_t historyCursor_ = 0;
    size_t historyCount_ = 0;
};

} // namespace vkr
