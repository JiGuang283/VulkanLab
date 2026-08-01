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
constexpr const char *kDockspaceName = "VulkanLab.DockSpace.v3";
constexpr const char *kViewportWindow = "Viewport###VulkanLab.Viewport";
constexpr const char *kScenesWindow = "Scenes###VulkanLab.Scenes";
constexpr const char *kAssetsWindow = "Assets###VulkanLab.Assets";
constexpr const char *kRenderWindow = "Render###VulkanLab.Render";
constexpr const char *kMaterialsWindow = "Materials###VulkanLab.Materials";
constexpr const char *kDiagnosticsWindow =
    "Diagnostics###VulkanLab.Diagnostics";
constexpr float kCompactLayoutWidth = 1100.0f;

const char *presetName(EditorWorkspacePreset preset) {
    switch (preset) {
    case EditorWorkspacePreset::Viewport:
        return "Viewport";
    case EditorWorkspacePreset::Debugging:
        return "Debugging";
    case EditorWorkspacePreset::Compact:
        return "Compact";
    }
    return "Viewport";
}

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
    const bool missingLayout =
        ImGui::DockBuilderGetNode(dockspaceId) == nullptr;
    if (missingLayout && !requestedPreset_) {
        requestedPreset_ = dockspaceSize.x < kCompactLayoutWidth
                               ? EditorWorkspacePreset::Compact
                               : EditorWorkspacePreset::Viewport;
    }
    if (resetLayoutRequested_ || requestedPreset_ || missingLayout) {
        const EditorWorkspacePreset preset =
            requestedPreset_.value_or(currentPreset_);
        buildDefaultLayout(dockspaceId, dockspacePosition.x,
                           dockspacePosition.y, dockspaceSize.x,
                           dockspaceSize.y, preset);
        currentPreset_ = preset;
        requestedPreset_.reset();
        resetLayoutRequested_ = false;
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f));
    ImGui::End();

    drawViewport(viewportFrame);
    drawPanel(kMaterialsWindow, materialsVisible_, panels.materials);
    drawPanel(kRenderWindow, renderVisible_, panels.render);
    drawPanel(kAssetsWindow, assetsVisible_, panels.assets);
    drawPanel(kScenesWindow, scenesVisible_, panels.scenes);
    drawPanel(kDiagnosticsWindow, diagnosticsVisible_, panels.diagnostics);
    if (activateDefaultTabsFrames_ == 1) {
        auto selectDockedWindow = [](const char *name) {
            ImGuiWindow *window = ImGui::FindWindowByName(name);
            if (!window || !window->DockNode)
                return;
            window->DockNode->SelectedTabId = window->TabId;
            if (window->DockNode->TabBar) {
                window->DockNode->TabBar->SelectedTabId = window->TabId;
                window->DockNode->TabBar->NextSelectedTabId = window->TabId;
            }
        };
        selectDockedWindow(kScenesWindow);
        if (currentPreset_ != EditorWorkspacePreset::Compact)
            selectDockedWindow(kRenderWindow);
        ImGui::SetWindowFocus(kViewportWindow);
    }
    if (activateDefaultTabsFrames_ > 0)
        --activateDefaultTabsFrames_;
}

void EditorDockWorkspace::buildDefaultLayout(unsigned int dockspaceId,
                                             float x, float y, float width,
                                             float height,
                                             EditorWorkspacePreset preset) {
    scenesVisible_ = true;
    assetsVisible_ = preset != EditorWorkspacePreset::Compact;
    renderVisible_ = true;
    materialsVisible_ = preset != EditorWorkspacePreset::Compact;
    diagnosticsVisible_ = preset == EditorWorkspacePreset::Debugging;
    viewportVisible_ = true;
    activateDefaultTabsFrames_ = 2;

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspaceId, ImVec2(x, y));
    ImGui::DockBuilderSetNodeSize(
        dockspaceId, ImVec2(std::max(width, 1.0f), std::max(height, 1.0f)));

    ImGuiID centerId = dockspaceId;
    ImGuiID leftId = 0;
    ImGuiID rightId = 0;
    const bool compact = preset == EditorWorkspacePreset::Compact;
    const float leftWidth = compact
                                ? std::clamp(width * 0.35f, 250.0f, 300.0f)
                                : std::clamp(width * 0.22f, 260.0f, 300.0f);
    const float leftRatio = std::clamp(
        leftWidth / std::max(width, 1.0f), 0.16f, compact ? 0.48f : 0.30f);

    ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, leftRatio, &leftId,
                                &centerId);
    if (compact) {
        ImGui::DockBuilderDockWindow(kScenesWindow, leftId);
        ImGui::DockBuilderDockWindow(kAssetsWindow, leftId);
        ImGui::DockBuilderDockWindow(kRenderWindow, leftId);
        ImGui::DockBuilderDockWindow(kMaterialsWindow, leftId);
        ImGui::DockBuilderDockWindow(kDiagnosticsWindow, leftId);
    } else {
        const float widthAfterLeft = std::max(width - leftWidth, 1.0f);
        const float rightWidth =
            std::clamp(width * 0.25f, 300.0f, 360.0f);
        const float rightRatio = std::clamp(
            rightWidth / widthAfterLeft, 0.20f, 0.46f);
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, rightRatio,
                                    &rightId, &centerId);
        ImGui::DockBuilderDockWindow(kScenesWindow, leftId);
        ImGui::DockBuilderDockWindow(kAssetsWindow, leftId);
        ImGui::DockBuilderDockWindow(kRenderWindow, rightId);
        ImGui::DockBuilderDockWindow(kMaterialsWindow, rightId);

        if (preset == EditorWorkspacePreset::Debugging) {
            ImGuiID bottomId = 0;
            ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.28f,
                                        &bottomId, &centerId);
            ImGui::DockBuilderDockWindow(kDiagnosticsWindow, bottomId);
        } else {
            ImGui::DockBuilderDockWindow(kDiagnosticsWindow, leftId);
        }
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
        for (const EditorWorkspacePreset preset : {
                 EditorWorkspacePreset::Viewport,
                 EditorWorkspacePreset::Debugging,
                 EditorWorkspacePreset::Compact}) {
            const bool selected = preset == currentPreset_;
            if (ImGui::MenuItem(presetName(preset), nullptr, selected))
                requestedPreset_ = preset;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Current Layout"))
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
    const bool open = ImGui::Begin(kViewportWindow, nullptr, flags);
    if (open) {
        const float statusHeight = ImGui::GetFrameHeight();
        const float statusStartY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        ImGui::SetCursorPosY(statusStartY + 2.0f);
        if (viewport.resizePending) {
            ImGui::TextDisabled("Render %u x %u  |  Resizing...",
                                viewport.renderWidth,
                                viewport.renderHeight);
        } else {
            ImGui::TextDisabled("Render %u x %u", viewport.renderWidth,
                                viewport.renderHeight);
        }
        ImGui::SetCursorPosY(statusStartY + statusHeight + 2.0f);
        ImGui::Separator();

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

    if (ImGui::Begin(name)) {
        ImGui::PushID(name);
        if (callback)
            callback();
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace vkr
