#include "EditorDockWorkspace.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#ifndef IMGUI_HAS_DOCK
#error "VulkanLab editor requires the Dear ImGui docking branch"
#endif

namespace vkr {
namespace {

constexpr const char *kHostWindow =
    "VulkanLab DockSpace###VulkanLab.DockHost";
constexpr const char *kDockspaceName = "VulkanLab.DockSpace.v2";
constexpr const char *kViewportWindow = "Viewport###VulkanLab.Viewport";
constexpr const char *kScenesWindow = "Scenes###VulkanLab.Scenes";
constexpr const char *kAssetsWindow = "Assets###VulkanLab.Assets";
constexpr const char *kRenderWindow = "Render###VulkanLab.Render";
constexpr const char *kMaterialsWindow = "Materials###VulkanLab.Materials";
constexpr const char *kDiagnosticsWindow =
    "Diagnostics###VulkanLab.Diagnostics";
constexpr float kCompactLayoutWidth = 1200.0f;

} // namespace

void EditorDockWorkspace::draw(const EditorFrameStatus &status,
                               const EditorViewportFrame &viewportFrame,
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

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f));
    ImGui::End();

    drawViewport(viewportFrame);
    drawPanel(kScenesWindow, scenesVisible_, panels.scenes);
    drawPanel(kAssetsWindow, assetsVisible_, panels.assets);
    drawPanel(kRenderWindow, renderVisible_, panels.render);
    drawPanel(kMaterialsWindow, materialsVisible_, panels.materials);
    drawPanel(kDiagnosticsWindow, diagnosticsVisible_, panels.diagnostics);
}

void EditorDockWorkspace::buildDefaultLayout(unsigned int dockspaceId,
                                             float x, float y, float width,
                                             float height) {
    scenesVisible_ = true;
    assetsVisible_ = true;
    renderVisible_ = true;
    materialsVisible_ = true;
    diagnosticsVisible_ = true;
    viewportVisible_ = true;

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
    ImGui::DockBuilderDockWindow(kViewportWindow, centerId);

    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockWorkspace::drawMenuBar(const EditorFrameStatus &status) {
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &viewportVisible_);
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

void EditorDockWorkspace::drawViewport(
    const EditorViewportFrame &viewport) {
    viewportState_ = {};
    if (!viewportVisible_)
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    const bool open = ImGui::Begin(kViewportWindow, &viewportVisible_, flags);
    if (open) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float width = std::max(available.x, 0.0f);
        const float height = std::max(available.y, 0.0f);
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;

        viewportState_.minX = origin.x;
        viewportState_.minY = origin.y;
        viewportState_.maxX = origin.x + width;
        viewportState_.maxY = origin.y + height;
        viewportState_.logicalWidth = width;
        viewportState_.logicalHeight = height;
        viewportState_.pixelWidth = static_cast<uint32_t>(std::lround(
            width * std::max(scale.x, 0.0f)));
        viewportState_.pixelHeight = static_cast<uint32_t>(std::lround(
            height * std::max(scale.y, 0.0f)));
        viewportState_.visible = true;
        viewportState_.valid = width > 0.0f && height > 0.0f &&
                               viewportState_.pixelWidth > 0 &&
                               viewportState_.pixelHeight > 0;
        viewportState_.focused = ImGui::IsWindowFocused(
            ImGuiFocusedFlags_RootAndChildWindows);

        if (viewportState_.valid) {
            if (viewport.textureId != 0) {
                ImGui::Image(
                    ImTextureRef(
                        static_cast<ImTextureID>(viewport.textureId)),
                    ImVec2(width, height));
            } else {
                ImGui::Dummy(ImVec2(width, height));
            }
            viewportState_.hovered = ImGui::IsItemHovered();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
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
