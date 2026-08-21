#include "EditorDockWorkspace.h"

#include "EditorIcons.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

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
constexpr const char *kDockspaceName = "VulkanLab.DockSpace.v4";
constexpr const char *kViewportWindow = "Viewport###VulkanLab.Viewport";
constexpr const char *kOutlinerWindow = "Outliner###VulkanLab.Outliner";
constexpr const char *kInspectorWindow = "Inspector###VulkanLab.Inspector";
constexpr const char *kContentWindow =
    "Content Browser###VulkanLab.ContentBrowser";
constexpr const char *kRenderWindow = "Render###VulkanLab.Render";
constexpr const char *kMaterialsWindow = "Materials###VulkanLab.Materials";
constexpr float kCompactLayoutWidth = 1100.0f;
constexpr float kStatusBarHeight = 28.0f;

} // namespace

EditorDockWorkspace::EditorDockWorkspace(
    EditorPreferencesStore &preferences)
    : preferences_(&preferences),
      currentPreset_(preferences.preferences().workspace) {
    applyPresetVisibility(currentPreset_);
}

void EditorDockWorkspace::applyPresetVisibility(
    EditorWorkspacePreset preset) {
    viewportVisible_ = true;
    outlinerVisible_ = preset != EditorWorkspacePreset::LookDev;
    inspectorVisible_ = true;
    contentBrowserVisible_ = true;
    renderVisible_ = preset != EditorWorkspacePreset::Scene;
    materialsVisible_ = preset != EditorWorkspacePreset::Scene;
}

void EditorDockWorkspace::draw(const EditorFrameStatus &status,
                               const EditorViewportFrame &viewportFrame,
                               const EditorPanelCallbacks &panels) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags hostFlags =
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

    drawMenuBar(panels);

    const EditorPreferences &preferences = preferences_->preferences();
    const bool drawerExpanded = preferences.bottomDrawerExpanded;
    const float drawerHeight = drawerExpanded
                                   ? std::clamp(preferences.bottomDrawerHeight,
                                                180.0f, 360.0f)
                                   : kStatusBarHeight;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float dockHeight = std::max(
        1.0f, available.y - drawerHeight - ImGui::GetStyle().ItemSpacing.y);
    ImGui::BeginChild("DockRegion", ImVec2(0.0f, dockHeight), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const ImGuiID dockspaceId = ImGui::GetID(kDockspaceName);
    const ImVec2 dockspacePosition = ImGui::GetCursorScreenPos();
    const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();
    const bool missingLayout =
        ImGui::DockBuilderGetNode(dockspaceId) == nullptr;
    if (missingLayout && !requestedPreset_) {
        requestedPreset_ = dockspaceSize.x < kCompactLayoutWidth
                               ? EditorWorkspacePreset::Compact
                               : currentPreset_;
    }
    if (resetLayoutRequested_ || requestedPreset_ || missingLayout) {
        const EditorWorkspacePreset preset =
            requestedPreset_.value_or(currentPreset_);
        buildDefaultLayout(dockspaceId, dockspacePosition.x,
                           dockspacePosition.y, dockspaceSize.x,
                           dockspaceSize.y, preset);
        currentPreset_ = preset;
        preferences_->edit().workspace = preset;
        requestedPreset_.reset();
        resetLayoutRequested_ = false;
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f));
    ImGui::EndChild();

    drawBottomDrawer(status, panels);
    ImGui::End();

    drawViewport(viewportFrame, panels);
    drawPanel(kOutlinerWindow, outlinerVisible_, panels.outliner);
    drawPanel(kInspectorWindow, inspectorVisible_, panels.inspector);
    drawPanel(kContentWindow, contentBrowserVisible_, panels.contentBrowser);
    drawPanel(kRenderWindow, renderVisible_, panels.render);
    drawPanel(kMaterialsWindow, materialsVisible_, panels.materials);

    if (activateDefaultTabsFrames_ == 1) {
        ImGui::SetWindowFocus(kViewportWindow);
    }
    if (activateDefaultTabsFrames_ > 0)
        --activateDefaultTabsFrames_;
}

void EditorDockWorkspace::requestPreset(EditorWorkspacePreset preset) {
    requestedPreset_ = preset;
    viewportMaximized_ = false;
    EditorPreferences &preferences = preferences_->edit();
    preferences.workspace = preset;
    preferences.bottomDrawerExpanded = preset == EditorWorkspacePreset::Debug;
}

void EditorDockWorkspace::toggleViewportMaximized() {
    viewportMaximized_ = !viewportMaximized_;
    requestedPreset_ = currentPreset_;
}

void EditorDockWorkspace::toggleBottomDrawer() {
    EditorPreferences &preferences = preferences_->edit();
    preferences.bottomDrawerExpanded = !preferences.bottomDrawerExpanded;
}

void EditorDockWorkspace::buildDefaultLayout(
    unsigned int dockspaceId, float x, float y, float width, float height,
    EditorWorkspacePreset preset) {
    applyPresetVisibility(preset);
    activateDefaultTabsFrames_ = 2;
    if (viewportMaximized_) {
        outlinerVisible_ = false;
        inspectorVisible_ = false;
        contentBrowserVisible_ = false;
        renderVisible_ = false;
        materialsVisible_ = false;
    }

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspaceId, ImVec2(x, y));
    ImGui::DockBuilderSetNodeSize(
        dockspaceId, ImVec2(std::max(width, 1.0f), std::max(height, 1.0f)));

    ImGuiID centerId = dockspaceId;
    if (viewportMaximized_) {
        ImGui::DockBuilderDockWindow(kViewportWindow, centerId);
        ImGui::DockBuilderFinish(dockspaceId);
        return;
    }

    if (preset == EditorWorkspacePreset::Compact) {
        ImGuiID sideId = 0;
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.30f, &sideId,
                                    &centerId);
        ImGui::DockBuilderDockWindow(kOutlinerWindow, sideId);
        ImGui::DockBuilderDockWindow(kInspectorWindow, sideId);
        ImGui::DockBuilderDockWindow(kContentWindow, sideId);
        ImGui::DockBuilderDockWindow(kRenderWindow, sideId);
        ImGui::DockBuilderDockWindow(kMaterialsWindow, sideId);
    } else if (preset == EditorWorkspacePreset::Scene) {
        ImGuiID leftId = 0;
        ImGuiID rightId = 0;
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, &leftId,
                                    &centerId);
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.26f,
                                    &rightId, &centerId);
        ImGuiID leftBottom = 0;
        ImGuiID leftTop = leftId;
        ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.42f,
                                    &leftBottom, &leftTop);
        ImGui::DockBuilderDockWindow(kOutlinerWindow, leftTop);
        ImGui::DockBuilderDockWindow(kContentWindow, leftBottom);
        ImGui::DockBuilderDockWindow(kInspectorWindow, rightId);
    } else if (preset == EditorWorkspacePreset::LookDev) {
        ImGuiID leftId = 0;
        ImGuiID rightId = 0;
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, &leftId,
                                    &centerId);
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.28f,
                                    &rightId, &centerId);
        ImGui::DockBuilderDockWindow(kContentWindow, leftId);
        ImGui::DockBuilderDockWindow(kOutlinerWindow, leftId);
        ImGuiID rightBottom = 0;
        ImGuiID rightTop = rightId;
        ImGui::DockBuilderSplitNode(rightTop, ImGuiDir_Down, 0.55f,
                                    &rightBottom, &rightTop);
        ImGui::DockBuilderDockWindow(kInspectorWindow, rightTop);
        ImGui::DockBuilderDockWindow(kRenderWindow, rightBottom);
        ImGui::DockBuilderDockWindow(kMaterialsWindow, rightBottom);
    } else {
        ImGuiID leftId = 0;
        ImGuiID rightId = 0;
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.25f, &leftId,
                                    &centerId);
        ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.25f,
                                    &rightId, &centerId);
        ImGui::DockBuilderDockWindow(kRenderWindow, leftId);
        ImGui::DockBuilderDockWindow(kContentWindow, leftId);
        ImGui::DockBuilderDockWindow(kInspectorWindow, rightId);
        ImGui::DockBuilderDockWindow(kMaterialsWindow, rightId);
        ImGui::DockBuilderDockWindow(kOutlinerWindow, rightId);
    }
    ImGui::DockBuilderDockWindow(kViewportWindow, centerId);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockWorkspace::drawMenuBar(
    const EditorPanelCallbacks &panels) {
    if (!ImGui::BeginMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", "Ctrl+N") && panels.newScene)
            panels.newScene();
        if (ImGui::MenuItem("Open Scene", "Ctrl+O") && panels.openScene)
            panels.openScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false,
                            panels.sceneSessionActive) && panels.saveScene)
            panels.saveScene();
        if (ImGui::MenuItem("Save As", "Ctrl+Shift+S", false,
                            panels.sceneSessionActive) && panels.saveSceneAs)
            panels.saveSceneAs();
        if (ImGui::MenuItem("Close Scene", nullptr, false,
                            panels.sceneSessionActive) && panels.closeScene)
            panels.closeScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Convert Model Preview", nullptr, false,
                            !panels.sceneSessionActive) &&
            panels.convertPreview)
            panels.convertPreview();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undoText = panels.undoLabel.empty()
                                         ? "Undo"
                                         : "Undo " + panels.undoLabel;
        const std::string redoText = panels.redoLabel.empty()
                                         ? "Redo"
                                         : "Redo " + panels.redoLabel;
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false,
                            panels.canUndo) && panels.undo)
            panels.undo();
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false,
                            panels.canRedo) && panels.redo)
            panels.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette", "Ctrl+P") &&
            panels.openCommandPalette)
            panels.openCommandPalette();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &viewportVisible_);
        ImGui::MenuItem("Outliner", nullptr, &outlinerVisible_);
        ImGui::MenuItem("Inspector", nullptr, &inspectorVisible_);
        ImGui::MenuItem("Content Browser", nullptr,
                        &contentBrowserVisible_);
        ImGui::MenuItem("Render", nullptr, &renderVisible_);
        ImGui::MenuItem("Materials", nullptr, &materialsVisible_);
        ImGui::Separator();
        if (ImGui::MenuItem("Bottom Drawer", nullptr,
                            preferences_->preferences()
                                .bottomDrawerExpanded))
            toggleBottomDrawer();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Workspace")) {
        for (const EditorWorkspacePreset preset : {
                 EditorWorkspacePreset::Scene,
                 EditorWorkspacePreset::LookDev,
                 EditorWorkspacePreset::Debug,
                 EditorWorkspacePreset::Compact}) {
            if (ImGui::MenuItem(editorWorkspaceName(preset), nullptr,
                                preset == currentPreset_))
                requestPreset(preset);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Current Layout"))
            resetLayoutRequested_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void EditorDockWorkspace::drawBottomDrawer(
    const EditorFrameStatus &status,
    const EditorPanelCallbacks &panels) {
    const EditorPreferences &preferences = preferences_->preferences();
    const bool expanded = preferences.bottomDrawerExpanded;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::BeginChild("BottomDrawer", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar);
    if (expanded) {
        ImGui::InvisibleButton("DrawerResize",
                               ImVec2(ImGui::GetContentRegionAvail().x, 3.0f));
        if (ImGui::IsItemActive()) {
            preferences_->edit().bottomDrawerHeight = std::clamp(
                preferences.bottomDrawerHeight - ImGui::GetIO().MouseDelta.y,
                180.0f, 360.0f);
        }
    }
    if (editor::iconButton("ToggleDrawer",
                           expanded ? icons::DrawerClose : icons::DrawerOpen,
                           expanded ? "v" : "^",
                           expanded ? "Collapse diagnostics"
                                    : "Expand diagnostics",
                           ImVec2(24.0f, 20.0f))) {
        preferences_->edit().bottomDrawerExpanded = !expanded;
    }
    ImGui::SameLine();
    const std::string sceneName =
        (status.sceneName.empty() ? "No Scene" : status.sceneName) +
        (panels.sceneDirty ? " *" : "");
    ImGui::TextUnformatted(sceneName.c_str());
    if (status.loading) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s %.0f%%", status.loadingLabel.c_str(),
                            std::clamp(status.loadingProgress, 0.0f, 1.0f) *
                                100.0f);
    }
    if (status.errorCount > 0) {
        ImGui::SameLine();
        ImGui::TextColored(editor::statusColor(editor::StatusTone::Error),
                           "%u errors", status.errorCount);
    }
    char metrics[96]{};
    if (status.gpuFrameMs >= 0.0f)
        std::snprintf(metrics, sizeof(metrics), "%.0f FPS | GPU %.2f ms",
                      status.fps, status.gpuFrameMs);
    else
        std::snprintf(metrics, sizeof(metrics), "%.0f FPS", status.fps);
    const float metricsWidth = ImGui::CalcTextSize(metrics).x;
    const float x = ImGui::GetWindowWidth() - metricsWidth - 12.0f;
    if (x > ImGui::GetCursorPosX()) {
        ImGui::SameLine(x);
        ImGui::TextDisabled("%s", metrics);
    }
    if (expanded && panels.bottomDrawer) {
        ImGui::Separator();
        ImGui::BeginChild("BottomDrawerContent", ImVec2(0.0f, 0.0f), false);
        panels.bottomDrawer();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void EditorDockWorkspace::drawViewport(
    const EditorViewportFrame &viewport,
    const EditorPanelCallbacks &panels) {
    viewportState_ = {};
    if (!viewportVisible_)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(
        kViewportWindow, nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (open) {
        const float startY = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8.0f,
                                  startY + 2.0f));
        if (panels.viewportToolbar)
            panels.viewportToolbar();
        char status[80]{};
        std::snprintf(status, sizeof(status), "%u x %u%s",
                      viewport.renderWidth, viewport.renderHeight,
                      viewport.resizePending ? " | Resizing" : "");
        const float width = ImGui::CalcTextSize(status).x;
        const float x = ImGui::GetWindowWidth() - width - 10.0f;
        if (x > ImGui::GetCursorPosX()) {
            ImGui::SameLine(x);
            ImGui::TextDisabled("%s", status);
        }
        ImGui::SetCursorPosY(startY + ImGui::GetFrameHeight() + 6.0f);
        ImGui::Separator();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float contentWidth = std::max(available.x, 0.0f);
        const float contentHeight = std::max(available.y, 0.0f);
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        viewportState_.minX = origin.x;
        viewportState_.minY = origin.y;
        viewportState_.maxX = origin.x + contentWidth;
        viewportState_.maxY = origin.y + contentHeight;
        viewportState_.logicalWidth = contentWidth;
        viewportState_.logicalHeight = contentHeight;
        viewportState_.pixelWidth = static_cast<uint32_t>(std::lround(
            contentWidth * std::max(scale.x, 0.0f)));
        viewportState_.pixelHeight = static_cast<uint32_t>(std::lround(
            contentHeight * std::max(scale.y, 0.0f)));
        viewportState_.visible = true;
        viewportState_.valid = contentWidth > 0.0f && contentHeight > 0.0f &&
                               viewportState_.pixelWidth > 0 &&
                               viewportState_.pixelHeight > 0;
        viewportState_.focused = ImGui::IsWindowFocused(
            ImGuiFocusedFlags_RootAndChildWindows);
        if (viewportState_.valid) {
            if (viewport.textureId != 0) {
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                                 viewport.textureId)),
                             ImVec2(contentWidth, contentHeight));
            } else {
                ImGui::Dummy(ImVec2(contentWidth, contentHeight));
            }
            viewportState_.hovered = ImGui::IsItemHovered();
            if (panels.viewportOverlay)
                panels.viewportOverlay(viewportState_);
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
