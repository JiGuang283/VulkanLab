#include "ScenesPanel.h"

#include "editor/EditorDragDrop.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/EditorWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vkr {
namespace {

bool containsIgnoreCase(const std::string &value,
                        const std::string &needle) {
    if (needle.empty())
        return true;
    std::string loweredValue = value;
    std::string loweredNeedle = needle;
    std::transform(loweredValue.begin(), loweredValue.end(),
                   loweredValue.begin(), [](char character) {
                       return static_cast<char>(std::tolower(
                           static_cast<unsigned char>(character)));
                   });
    std::transform(loweredNeedle.begin(), loweredNeedle.end(),
                   loweredNeedle.begin(), [](char character) {
                       return static_cast<char>(std::tolower(
                           static_cast<unsigned char>(character)));
                   });
    return loweredValue.find(loweredNeedle) != std::string::npos;
}

editor::StatusTone statusTone(const std::string &state, bool available) {
    if (!available || state == "Invalid" || state == "Failed")
        return editor::StatusTone::Error;
    if (state == "Ready" || state == "Valid")
        return editor::StatusTone::Success;
    if (state == "Importing")
        return editor::StatusTone::Info;
    if (state == "Missing" || state == "Stale" || state == "Warnings" ||
        state == "Unavailable")
        return editor::StatusTone::Warning;
    return editor::StatusTone::Neutral;
}

std::string compactLabel(const std::string &value, size_t limit = 24) {
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit - 3) + "...";
}

} // namespace

void ScenesPanel::draw(const SceneWorkflowSnapshot &snapshot,
                       const SceneWorkflowActions &actions,
                       bool modelsOnly, ContentBrowserViewMode viewMode) {
    if (!modelsOnly) {
        ImGui::SetNextItemWidth(-92.0f);
        ImGui::InputTextWithHint("##SceneSearch", "Search scenes...",
                                 search_.data(), search_.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(snapshot.busy);
        if (ImGui::Button("Refresh") && actions.refresh)
            actions.refresh();
        ImGui::EndDisabled();

        if (snapshot.nativeScenes.empty()) {
            editor::emptyState("No native scenes in the Catalog.");
            return;
        }
        ImGui::BeginChild("NativeSceneBrowser", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders);
        const float tileWidth = 150.0f;
        const int columns = std::max(
            1, static_cast<int>(ImGui::GetContentRegionAvail().x /
                                (tileWidth + ImGui::GetStyle().ItemSpacing.x)));
        int visibleIndex = 0;
        for (const SceneWorkflowItemSnapshot &item : snapshot.nativeScenes) {
            if (!containsIgnoreCase(item.displayName, search_.data()) &&
                !containsIgnoreCase(item.id, search_.data())) {
                continue;
            }
            ImGui::PushID(item.id.c_str());
            ImGui::BeginDisabled(!item.available);
            const bool selected = item.index == snapshot.selectedIndex;
            bool activated = false;
            if (viewMode == ContentBrowserViewMode::Grid) {
                std::string label;
                if (editor::iconsAvailable()) {
                    label = icons::Scene;
                    label += "\n";
                }
                label += compactLabel(item.displayName);
                if (selected)
                    ImGui::PushStyleColor(
                        ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                activated = ImGui::Button(label.c_str(),
                                          ImVec2(tileWidth, 58.0f));
                if (selected)
                    ImGui::PopStyleColor();
            } else {
                std::string label;
                if (editor::iconsAvailable()) {
                    label = icons::Scene;
                    label += "  ";
                }
                label += item.displayName;
                activated = ImGui::Selectable(label.c_str(), selected);
            }
            if (activated) {
                if (actions.selectModel)
                    actions.selectModel(item.index);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    actions.loadSceneDocument) {
                    actions.loadSceneDocument(item.index);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s\n%s", item.id.c_str(),
                                  item.available
                                      ? "Double-click to open"
                                      : item.unavailableReason.c_str());
            }
            if (item.current) {
                ImGui::SameLine();
                ImGui::TextDisabled("Open");
            }
            if (viewMode == ContentBrowserViewMode::Grid &&
                (++visibleIndex % columns) != 0)
                ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::EndChild();
        return;
    }

    if (snapshot.showImport) {
        ImGui::BeginDisabled(snapshot.busy || !snapshot.canImport);
        if (ImGui::Button("Import Model...") && actions.beginModelImport)
            actions.beginModelImport();
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(snapshot.busy);
    if (ImGui::Button("Refresh") && actions.refresh)
        actions.refresh();
    ImGui::EndDisabled();

    if (!snapshot.status.empty())
        editor::statusIndicator(snapshot.status.c_str(),
                                editor::StatusTone::Info);
    if (!snapshot.error.empty())
        editor::statusIndicator(snapshot.error.c_str(),
                                editor::StatusTone::Error);
    if (snapshot.copyActive) {
        ImGui::ProgressBar(std::clamp(snapshot.copyProgress, 0.0f, 1.0f));
        if (!snapshot.copyFile.empty())
            ImGui::Text("Copying: %s", snapshot.copyFile.c_str());
        if (ImGui::Button("Cancel Import") && actions.cancelImport)
            actions.cancelImport();
    }

    if (!snapshot.enginePrimitives.empty()) {
        ImGui::SeparatorText("Engine Primitives");
        ImGui::BeginChild("EnginePrimitiveBrowser", ImVec2(0.0f, 92.0f),
                          ImGuiChildFlags_Borders);
        const float itemWidth = std::max(96.0f,
                                         ImGui::GetContentRegionAvail().x /
                                             3.0f - 6.0f);
        for (size_t index = 0; index < snapshot.enginePrimitives.size();
             ++index) {
            const EnginePrimitiveItemSnapshot &primitive =
                snapshot.enginePrimitives[index];
            ImGui::PushID(primitive.id.c_str());
            ImGui::BeginDisabled(!primitive.canInstantiate);
            std::string primitiveLabel;
            if (editor::iconsAvailable()) {
                primitiveLabel = icons::Box;
                primitiveLabel += "  ";
            }
            primitiveLabel += primitive.displayName;
            ImGui::Button(primitiveLabel.c_str(),
                          ImVec2(itemWidth, 30.0f));
            ImGui::EndDisabled();
            if (primitive.canInstantiate && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    editor::kModelAssetPayload, primitive.id.c_str(),
                    primitive.id.size() + 1);
                ImGui::TextUnformatted(primitive.displayName.c_str());
                ImGui::TextDisabled("Drop into the Viewport");
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    primitive.canInstantiate
                        ? "Drag into the Viewport to create an instance"
                        : "Open a writable native scene to instantiate");
            }
            if ((index + 1) % 3 != 0)
                ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::SeparatorText("Model Previews");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ModelSearch", "Search model previews...",
                             search_.data(), search_.size());
    const std::string search = search_.data();
    const float listHeight = std::clamp(
        ImGui::GetContentRegionAvail().y * 0.46f, 150.0f, 310.0f);
    ImGui::BeginChild("ModelPreviewBrowser", ImVec2(0.0f, listHeight),
                      ImGuiChildFlags_Borders);
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (viewMode == ContentBrowserViewMode::List &&
        ImGui::BeginTable("ModelPreviewTable", 2, flags)) {
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch,
                                0.72f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                                76.0f);
        ImGui::TableHeadersRow();
        for (const SceneWorkflowItemSnapshot &item : snapshot.models) {
            if (!containsIgnoreCase(item.displayName, search) &&
                !containsIgnoreCase(item.id, search))
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string label = item.displayName + "###Model" + item.id;
            ImGui::BeginDisabled(!item.available);
            if (ImGui::Selectable(label.c_str(),
                                  item.index == snapshot.selectedIndex,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                if (actions.selectModel)
                    actions.selectModel(item.index);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    actions.loadPreview)
                    actions.loadPreview(item.index);
            }
            ImGui::EndDisabled();
            if (item.canInstantiate && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    editor::kModelAssetPayload, item.id.c_str(),
                    item.id.size() + 1);
                ImGui::TextUnformatted(item.displayName.c_str());
                ImGui::TextDisabled("Drop into the Viewport");
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (item.canInstantiate) {
                    ImGui::SetTooltip(
                        "%s\nDouble-click to load preview\nDrag into the "
                        "Viewport to instantiate",
                        item.displayName.c_str());
                } else {
                    ImGui::SetTooltip(
                        "%s%s%s", item.displayName.c_str(),
                        item.available ? "\nDouble-click to load" : "\n",
                        item.available ? ""
                                       : item.unavailableReason.c_str());
                }
            }
            ImGui::TableSetColumnIndex(1);
            const char *state = item.available ? item.artifactState.c_str()
                                               : "Blocked";
            const char *reason = item.available
                                     ? (item.artifactReason.empty()
                                            ? nullptr
                                            : item.artifactReason.c_str())
                                     : item.unavailableReason.c_str();
            editor::statusIndicator(state,
                                    statusTone(item.artifactState,
                                               item.available),
                                    reason);
        }
        ImGui::EndTable();
    } else if (viewMode == ContentBrowserViewMode::Grid) {
        const float tileWidth = 150.0f;
        const int columns = std::max(
            1, static_cast<int>(ImGui::GetContentRegionAvail().x /
                                (tileWidth + ImGui::GetStyle().ItemSpacing.x)));
        int visibleIndex = 0;
        for (const SceneWorkflowItemSnapshot &item : snapshot.models) {
            if (!containsIgnoreCase(item.displayName, search) &&
                !containsIgnoreCase(item.id, search))
                continue;
            ImGui::PushID(item.id.c_str());
            ImGui::BeginDisabled(!item.available);
            std::string label;
            if (editor::iconsAvailable()) {
                label = icons::Model;
                label += "\n";
            }
            label += compactLabel(item.displayName);
            const bool selected = item.index == snapshot.selectedIndex;
            if (selected)
                ImGui::PushStyleColor(
                    ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const bool activated =
                ImGui::Button(label.c_str(), ImVec2(tileWidth, 64.0f));
            if (selected)
                ImGui::PopStyleColor();
            if (activated && actions.selectModel)
                actions.selectModel(item.index);
            if (activated && ImGui::IsMouseDoubleClicked(
                                 ImGuiMouseButton_Left) &&
                actions.loadPreview)
                actions.loadPreview(item.index);
            ImGui::EndDisabled();
            if (item.canInstantiate && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    editor::kModelAssetPayload, item.id.c_str(),
                    item.id.size() + 1);
                ImGui::TextUnformatted(item.displayName.c_str());
                ImGui::TextDisabled("Drop into the Viewport");
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s\n%s", item.id.c_str(),
                                  item.artifactState.c_str());
            if ((++visibleIndex % columns) != 0)
                ImGui::SameLine();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    const auto selected = std::find_if(
        snapshot.models.begin(), snapshot.models.end(),
        [&](const SceneWorkflowItemSnapshot &item) {
            return item.index == snapshot.selectedIndex;
        });
    if (selected != snapshot.models.end()) {
        const SceneWorkflowItemSnapshot &item = *selected;
        ImGui::SeparatorText("Selected Model Preview");
        if (editor::beginPropertyGrid("SelectedModelProperties", 0.34f)) {
            editor::propertyLabel("Name");
            ImGui::TextUnformatted(item.displayName.c_str());
            editor::propertyLabel("ID");
            ImGui::TextUnformatted(item.id.c_str());
            editor::propertyLabel("Profile");
            ImGui::TextUnformatted(item.profileId.c_str());
            if (!item.encoder.empty()) {
                editor::propertyLabel("Encoder");
                ImGui::TextUnformatted(item.encoder.c_str());
            }
            if (!item.sourcePath.empty()) {
                editor::propertyLabel("Source");
                editor::pathValue(item.sourcePath);
            }
            if (!item.validationState.empty()) {
                editor::propertyLabel("Validation");
                editor::statusIndicator(
                    item.validationState.c_str(),
                    statusTone(item.validationState, true),
                    item.validationReason.empty()
                        ? nullptr
                        : item.validationReason.c_str());
            }
            editor::endPropertyGrid();
        }

        ImGui::BeginDisabled(!item.available);
        if (ImGui::Button("Load Preview", ImVec2(112.0f, 0.0f)) &&
            actions.loadPreview)
            actions.loadPreview(item.index);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("..."))
            ImGui::OpenPopup("ModelPreviewActions");
        if (ImGui::BeginPopup("ModelPreviewActions")) {
            if (ImGui::MenuItem("Reimport", nullptr, false,
                                item.canAuthor) &&
                actions.reimportModel)
                actions.reimportModel(item.index);
            const std::string validateLabel =
                item.validationState.empty() ||
                        item.validationState == "NotChecked"
                    ? "Validate"
                    : "Revalidate";
            if (ImGui::MenuItem(validateLabel.c_str(), nullptr, false,
                                item.canAuthor) &&
                actions.validateModel)
                actions.validateModel(item.index);
            if (ImGui::MenuItem("Open Validation Report", nullptr, false,
                                !item.validationReportPath.empty()) &&
                actions.openReport)
                actions.openReport(item.validationReportPath);
            ImGui::Separator();
            if (ImGui::MenuItem("Load Source Fallback", nullptr, false,
                                item.canLoadSource) &&
                actions.loadSourceFallback)
                actions.loadSourceFallback(item.index);
            if (ImGui::MenuItem("Save Preview Camera", nullptr, false,
                                item.canEditCatalog) &&
                actions.savePreviewCamera)
                actions.savePreviewCamera(item.index);
            if (ImGui::MenuItem("Remove Model From Catalog", nullptr, false,
                                item.canEditCatalog && !item.current))
                pendingRemoveIndex_ = item.index;
            ImGui::EndPopup();
        }
        if (pendingRemoveIndex_ >= 0)
            ImGui::OpenPopup("Remove Model");
        if (ImGui::BeginPopupModal("Remove Model", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Remove this model from the Catalog? Source files and "
                "derived artifacts will not be deleted.");
            if (ImGui::Button("Remove")) {
                if (actions.removeModel)
                    actions.removeModel(pendingRemoveIndex_);
                pendingRemoveIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pendingRemoveIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    drawImportDialog(snapshot, actions);
}

void ScenesPanel::drawImportDialog(
    const SceneWorkflowSnapshot &snapshot,
    const SceneWorkflowActions &actions) {
    if (snapshot.openImportDialog && snapshot.importPreflight) {
        std::snprintf(importDisplayName_.data(), importDisplayName_.size(),
                      "%s",
                      snapshot.importPreflight->suggestedDisplayName.c_str());
        std::snprintf(importModelId_.data(), importModelId_.size(), "%s",
                      snapshot.importPreflight->suggestedModelId.c_str());
        importProfileIndex_ = snapshot.defaultImportProfileIndex;
        importReferenceExisting_ = snapshot.defaultReferenceExisting;
        importLoadAfter_ = snapshot.defaultLoadAfterImport;
        importAllowUnvalidated_ = false;
        ImGui::OpenPopup("Import Model");
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float availableWidth =
        std::max(260.0f, viewport->WorkSize.x - 24.0f);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(420.0f, availableWidth), 0.0f),
        ImVec2(availableWidth,
               std::max(320.0f, viewport->WorkSize.y - 24.0f)));
    if (!ImGui::BeginPopupModal("Import Model", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    bool validationAllowsImport = false;
    if (snapshot.importPreflight) {
        ImGui::TextWrapped("Source: %s",
                           snapshot.importPreflight->sourcePath.string().c_str());
        ImGui::Text("Dependencies: %llu",
                    static_cast<unsigned long long>(
                        snapshot.importPreflight->dependencies.size()));
    }
    if (snapshot.importValidation) {
        const AssetValidationReport &report = *snapshot.importValidation;
        ImGui::SeparatorText("glTF Validation");
        ImGui::Text("State: %s", assetValidationStateName(report.state));
        ImGui::Text("Errors %llu  Warnings %llu  Info %llu  Hints %llu",
                    static_cast<unsigned long long>(report.errorCount),
                    static_cast<unsigned long long>(report.warningCount),
                    static_cast<unsigned long long>(report.infoCount),
                    static_cast<unsigned long long>(report.hintCount));
        if (!report.issues.empty() && ImGui::TreeNode("Validator Issues")) {
            const size_t count = std::min<size_t>(report.issues.size(), 50);
            ImGui::BeginChild("ValidationIssues", ImVec2(0.0f, 220.0f),
                              ImGuiChildFlags_Borders);
            for (size_t index = 0; index < count; ++index) {
                const AssetValidationIssue &issue = report.issues[index];
                ImGui::TextWrapped("[%u] %s: %s", issue.severity,
                                   issue.code.c_str(), issue.message.c_str());
                if (!issue.pointer.empty())
                    ImGui::TextDisabled("%s", issue.pointer.c_str());
                ImGui::Separator();
            }
            ImGui::EndChild();
            ImGui::TreePop();
        }
        if (!snapshot.importValidationReportPath.empty() &&
            ImGui::Button("Open Report") && actions.openReport)
            actions.openReport(snapshot.importValidationReportPath);
        validationAllowsImport =
            report.state == AssetValidationState::Valid ||
            report.state == AssetValidationState::Warnings;
        if (report.state == AssetValidationState::Unavailable) {
            ImGui::Checkbox("Import without validation",
                            &importAllowUnvalidated_);
            validationAllowsImport = importAllowUnvalidated_;
        }
    }

    ImGui::SeparatorText("Import Settings");
    ImGui::InputText("Name", importDisplayName_.data(),
                     importDisplayName_.size());
    ImGui::InputText("Model ID", importModelId_.data(), importModelId_.size());
    if (!snapshot.importProfileIds.empty()) {
        importProfileIndex_ = std::clamp(
            importProfileIndex_, 0,
            static_cast<int>(snapshot.importProfileIds.size()) - 1);
        const char *current =
            snapshot.importProfileIds[importProfileIndex_].c_str();
        if (ImGui::BeginCombo("Import Profile", current)) {
            for (int index = 0;
                 index < static_cast<int>(snapshot.importProfileIds.size());
                 ++index) {
                if (ImGui::Selectable(snapshot.importProfileIds[index].c_str(),
                                      index == importProfileIndex_))
                    importProfileIndex_ = index;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::BeginDisabled(!snapshot.defaultReferenceExisting);
    ImGui::Checkbox("Reference existing project file",
                    &importReferenceExisting_);
    ImGui::EndDisabled();
    if (!snapshot.defaultReferenceExisting)
        importReferenceExisting_ = false;
    ImGui::Checkbox("Load preview after import", &importLoadAfter_);

    const bool valid = validationAllowsImport && importDisplayName_[0] != '\0' &&
                       isStableAssetId(importModelId_.data()) &&
                       !snapshot.importProfileIds.empty();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Import") && actions.confirmImport) {
        actions.confirmImport(
            {importDisplayName_.data(), importModelId_.data(),
             snapshot.importProfileIds[importProfileIndex_],
             importReferenceExisting_, importLoadAfter_,
             importAllowUnvalidated_});
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        if (actions.dismissImport)
            actions.dismissImport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace vkr
