#include "EditorDockWorkspace.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <string>

#ifndef IMGUI_HAS_DOCK
#error "VulkanLab editor requires the Dear ImGui docking branch"
#endif

namespace vkr {
namespace {

constexpr const char *kHostWindow =
    "VulkanLab DockSpace###VulkanLab.DockHost";
constexpr const char *kDockspaceName = "VulkanLab.DockSpace.v1";
constexpr const char *kScenesWindow = "Scenes###VulkanLab.Scenes";
constexpr const char *kAssetsWindow = "Assets###VulkanLab.Assets";
constexpr const char *kRenderWindow = "Render###VulkanLab.Render";
constexpr const char *kMaterialsWindow = "Materials###VulkanLab.Materials";
constexpr const char *kDiagnosticsWindow =
    "Diagnostics###VulkanLab.Diagnostics";
constexpr float kCompactLayoutWidth = 1200.0f;

} // namespace

void EditorDockWorkspace::draw(const EditorFrameStatus &status,
                               const EditorPanelCallbacks &panels) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(kHostWindow, nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    drawMenuBar(status);

    const ImGuiID dockspaceId = ImGui::GetID(kDockspaceName);
    const ImVec2 dockspacePosition = ImGui::GetCursorScreenPos();
    const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();
    if (resetLayoutRequested_ ||
        ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        buildDefaultLayout(dockspaceId, dockspacePosition.x,
                           dockspacePosition.y, dockspaceSize.x,
                           dockspaceSize.y);
        resetLayoutRequested_ = false;
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    if (const ImGuiDockNode *central =
            ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        sceneArea_ = {central->Pos.x,
                      central->Pos.y,
                      central->Pos.x + central->Size.x,
                      central->Pos.y + central->Size.y,
                      central->Size.x > 0.0f && central->Size.y > 0.0f};
    } else {
        sceneArea_.valid = false;
    }
    ImGui::End();

    drawPanel(kScenesWindow, scenesVisible_, panels.scenes);
    drawPanel(kAssetsWindow, assetsVisible_, panels.assets);
    drawPanel(kRenderWindow, renderVisible_, panels.render);
    drawPanel(kMaterialsWindow, materialsVisible_, panels.materials);
    drawPanel(kDiagnosticsWindow, diagnosticsVisible_, panels.diagnostics);
}

bool EditorDockWorkspace::sceneAreaHovered() const {
    if (!sceneArea_.valid)
        return false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    return mouse.x >= sceneArea_.minX && mouse.x < sceneArea_.maxX &&
           mouse.y >= sceneArea_.minY && mouse.y < sceneArea_.maxY;
}

void EditorDockWorkspace::buildDefaultLayout(unsigned int dockspaceId,
                                             float x, float y, float width,
                                             float height) {
    scenesVisible_ = true;
    assetsVisible_ = true;
    renderVisible_ = true;
    materialsVisible_ = true;
    diagnosticsVisible_ = true;

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspaceId, ImVec2(x, y));
    ImGui::DockBuilderSetNodeSize(
        dockspaceId, ImVec2(std::max(width, 1.0f), std::max(height, 1.0f)));

    ImGuiID centerId = dockspaceId;
    ImGuiID leftId = 0;
    ImGuiID rightId = 0;
    const bool compact = width < kCompactLayoutWidth;
    const float leftWidth =
        compact ? std::clamp(width * 0.30f, 220.0f, 300.0f)
                : std::clamp(width * 0.22f, 260.0f, 360.0f);
    const float leftRatio =
        std::clamp(leftWidth / std::max(width, 1.0f), 0.15f, 0.45f);
    const float widthAfterLeft = std::max(width - leftWidth, 1.0f);
    const float rightWidth =
        compact ? std::clamp(width * 0.42f, 320.0f, 360.0f)
                : std::clamp(width * 0.30f, 380.0f, 440.0f);
    const float rightRatio =
        std::clamp(rightWidth / widthAfterLeft, 0.20f, 0.60f);

    ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, leftRatio, &leftId,
                                &centerId);
    ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, rightRatio,
                                &rightId, &centerId);

    ImGui::DockBuilderDockWindow(kScenesWindow, leftId);
    ImGui::DockBuilderDockWindow(kAssetsWindow, leftId);
    ImGui::DockBuilderDockWindow(kRenderWindow, rightId);
    ImGui::DockBuilderDockWindow(kMaterialsWindow, rightId);

    if (compact) {
        ImGui::DockBuilderDockWindow(kDiagnosticsWindow, leftId);
    } else {
        ImGuiID bottomId = 0;
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.24f,
                                    &bottomId, &centerId);
        ImGui::DockBuilderDockWindow(kDiagnosticsWindow, bottomId);
    }

    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockWorkspace::drawMenuBar(const EditorFrameStatus &status) {
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Scenes", nullptr, &scenesVisible_);
        ImGui::MenuItem("Assets", nullptr, &assetsVisible_);
        ImGui::MenuItem("Render", nullptr, &renderVisible_);
        ImGui::MenuItem("Materials", nullptr, &materialsVisible_);
        ImGui::MenuItem("Diagnostics", nullptr, &diagnosticsVisible_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Reset Layout"))
            resetLayoutRequested_ = true;
        ImGui::EndMenu();
    }

    ImGui::Separator();
    const std::string sceneName =
        status.sceneName.empty() ? "No Scene" : status.sceneName;
    ImGui::TextUnformatted(sceneName.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", sceneName.c_str());

    char metrics[160]{};
    if (status.loading) {
        std::snprintf(metrics, sizeof(metrics), "%s %.0f%% | %.0f FPS",
                      status.loadingLabel.c_str(),
                      std::clamp(status.loadingProgress, 0.0f, 1.0f) * 100.0f,
                      status.fps);
    } else if (status.gpuFrameMs >= 0.0f) {
        std::snprintf(metrics, sizeof(metrics), "%.0f FPS | GPU %.2f ms",
                      status.fps, status.gpuFrameMs);
    } else {
        std::snprintf(metrics, sizeof(metrics), "%.0f FPS", status.fps);
    }

    const float metricsWidth = ImGui::CalcTextSize(metrics).x;
    const float rightAlignedX =
        ImGui::GetWindowWidth() - metricsWidth -
        ImGui::GetStyle().WindowPadding.x;
    if (rightAlignedX > ImGui::GetCursorPosX() +
                            ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine(rightAlignedX);
        ImGui::TextUnformatted(metrics);
    }

    ImGui::EndMenuBar();
}

void EditorDockWorkspace::drawPanel(
    const char *name, bool &visible,
    const std::function<void()> &callback) {
    if (!visible)
        return;

    if (ImGui::Begin(name, &visible)) {
        ImGui::PushID(name);
        if (callback)
            callback();
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace vkr
