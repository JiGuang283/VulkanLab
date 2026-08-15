#include "AssetsPanel.h"

#include "editor/EditorWidgets.h"
#include "scene_data/SceneIds.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace vkr {
namespace {

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

editor::StatusTone statusTone(const std::string &state) {
    if (state == "Ready" || state == "Valid")
        return editor::StatusTone::Success;
    if (state == "Importing" || state == "Running" || state == "Queued")
        return editor::StatusTone::Info;
    if (state == "Invalid" || state == "Failed" || state == "Cancelled")
        return editor::StatusTone::Error;
    if (state == "Missing" || state == "Stale" || state == "Warnings")
        return editor::StatusTone::Warning;
    return editor::StatusTone::Neutral;
}

} // namespace

void AssetsPanel::draw(const AssetWorkflowSnapshot &snapshot,
                       const AssetsPanelActions &actions,
                       bool environmentsOnly) {
    if (!environmentsOnly) {
    ImGui::SeparatorText("Project Cache");
    if (editor::beginPropertyGrid("AssetProjectSummary", 0.34f)) {
        editor::propertyLabel("Project");
        ImGui::TextUnformatted(snapshot.projectId.c_str());
        editor::propertyLabel("Mode");
        ImGui::TextUnformatted(snapshot.mode.c_str());
        editor::propertyLabel("Catalog");
        editor::pathValue(snapshot.catalogPath);
        editor::propertyLabel("Cache");
        editor::pathValue(snapshot.cachePath);
        if (snapshot.hasUsage) {
            editor::propertyLabel("Index");
            ImGui::Text("%llu records / %llu ready",
                        static_cast<unsigned long long>(snapshot.indexRecords),
                        static_cast<unsigned long long>(snapshot.readyRecords));
            editor::propertyLabel("Cache Blobs");
            ImGui::Text("%llu / %.2f MiB",
                        static_cast<unsigned long long>(snapshot.cacheBlobFiles),
                        bytesToMiB(snapshot.cacheBlobBytes));
            editor::propertyLabel("Unreferenced");
            ImGui::Text("%llu / %.2f MiB",
                        static_cast<unsigned long long>(
                            snapshot.unreferencedBlobFiles),
                        bytesToMiB(snapshot.unreferencedBlobBytes));
        }
        editor::endPropertyGrid();
    }

    if (snapshot.selectedModel) {
        const AssetArtifactSnapshot &artifact = *snapshot.selectedModel;
        ImGui::SeparatorText("Selected Model Artifacts");
        if (editor::beginPropertyGrid("SelectedArtifactSummary", 0.34f)) {
            editor::propertyLabel("Model");
            ImGui::TextUnformatted(artifact.modelName.c_str());
            editor::propertyLabel("Profile");
            ImGui::TextUnformatted(artifact.profileId.c_str());
            if (!artifact.encoder.empty()) {
                editor::propertyLabel("Encoder");
                ImGui::TextUnformatted(artifact.encoder.c_str());
            }
            editor::propertyLabel("Artifacts");
            editor::statusIndicator(
                artifact.state.c_str(), statusTone(artifact.state),
                artifact.reason.empty() ? nullptr : artifact.reason.c_str());
            if (!artifact.payloadKind.empty()) {
                editor::propertyLabel("Payload");
                ImGui::TextUnformatted(artifact.payloadKind.c_str());
            }
            if (artifact.entryCount > 0) {
                editor::propertyLabel("Blobs");
                ImGui::Text("%llu / %.2f MiB",
                            static_cast<unsigned long long>(artifact.entryCount),
                            bytesToMiB(artifact.blobBytes));
            }
            editor::endPropertyGrid();
        }
        if (!artifact.failureCode.empty()) {
            ImGui::Text("Last Failure: %s", artifact.failureCode.c_str());
            ImGui::TextWrapped("%s", artifact.failureMessage.c_str());
        }
    }

    }

    if (environmentsOnly) {
    ImGui::SeparatorText("Environments");
    const auto selected = std::find_if(
        snapshot.environments.begin(), snapshot.environments.end(),
        [&](const EnvironmentAssetSnapshot &environment) {
            return environment.id == selectedEnvironmentId_;
        });
    const char *selectedName = selected == snapshot.environments.end()
                                   ? "None"
                                   : selected->displayName.c_str();
    if (ImGui::BeginCombo("Environment Asset", selectedName)) {
        if (ImGui::Selectable("None", selectedEnvironmentId_.empty()))
            selectedEnvironmentId_.clear();
        for (const EnvironmentAssetSnapshot &environment :
             snapshot.environments) {
            const bool isSelected =
                selectedEnvironmentId_ == environment.id;
            if (ImGui::Selectable(environment.displayName.c_str(), isSelected))
                selectedEnvironmentId_ = environment.id;
        }
        ImGui::EndCombo();
    }

    const auto selectedAfterCombo = std::find_if(
        snapshot.environments.begin(), snapshot.environments.end(),
        [&](const EnvironmentAssetSnapshot &environment) {
            return environment.id == selectedEnvironmentId_;
        });
    if (selectedAfterCombo != snapshot.environments.end()) {
        const EnvironmentAssetSnapshot &environment = *selectedAfterCombo;
        ImGui::Text("Profile: %s", environment.profileId.c_str());
        ImGui::TextWrapped("Source: %s", environment.source.c_str());
        editor::statusIndicator(
            environment.artifactState.c_str(),
            statusTone(environment.artifactState),
            environment.artifactReason.empty()
                ? nullptr
                : environment.artifactReason.c_str());
        if (environment.entryCount > 0) {
            ImGui::Text("Blobs: %llu (%.2f MiB)",
                        static_cast<unsigned long long>(environment.entryCount),
                        bytesToMiB(environment.blobBytes));
        }
        ImGui::BeginDisabled(!snapshot.canBuildEnvironments);
        if (ImGui::Button(environment.ready ? "Rebuild" : "Build") &&
            actions.buildEnvironment)
            actions.buildEnvironment(environment.id, environment.ready);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!snapshot.canEditEnvironments);
        if (ImGui::Button("Remove"))
            pendingRemoveEnvironmentId_ = environment.id;
        ImGui::EndDisabled();
    }

    ImGui::BeginDisabled(!snapshot.canEditEnvironments);
    if (ImGui::Button("Import HDR...") && actions.chooseEnvironment) {
        importDraft_ = actions.chooseEnvironment();
        if (importDraft_) {
            std::snprintf(importDisplayName_.data(), importDisplayName_.size(),
                          "%s", importDraft_->displayName.c_str());
            std::snprintf(importEnvironmentId_.data(), importEnvironmentId_.size(),
                          "%s", importDraft_->environmentId.c_str());
            importProfileIndex_ = 0;
            ImGui::OpenPopup("Import HDR Environment");
        }
    }
    ImGui::EndDisabled();

    if (!pendingRemoveEnvironmentId_.empty())
        ImGui::OpenPopup("Remove Environment");
    if (ImGui::BeginPopupModal("Remove Environment", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Remove this environment from the Catalog? Source and derived "
            "artifacts will be retained.");
        if (ImGui::Button("Remove")) {
            if (actions.removeEnvironment)
                actions.removeEnvironment(pendingRemoveEnvironmentId_);
            selectedEnvironmentId_.clear();
            pendingRemoveEnvironmentId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingRemoveEnvironmentId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Import HDR Environment", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (importDraft_) {
            ImGui::TextWrapped("Source: %s",
                               importDraft_->source.string().c_str());
            ImGui::InputText("Name", importDisplayName_.data(),
                             importDisplayName_.size());
            ImGui::InputText("Environment ID", importEnvironmentId_.data(),
                             importEnvironmentId_.size());
            if (!importDraft_->profileIds.empty()) {
                importProfileIndex_ = std::clamp(
                    importProfileIndex_, 0,
                    static_cast<int>(importDraft_->profileIds.size()) - 1);
                const char *current =
                    importDraft_->profileIds[importProfileIndex_].c_str();
                if (ImGui::BeginCombo("IBL Profile", current)) {
                    for (int index = 0;
                         index < static_cast<int>(
                                     importDraft_->profileIds.size());
                         ++index) {
                        if (ImGui::Selectable(
                                importDraft_->profileIds[index].c_str(),
                                index == importProfileIndex_))
                            importProfileIndex_ = index;
                    }
                    ImGui::EndCombo();
                }
            }
            const bool valid = importDisplayName_[0] != '\0' &&
                               isStableAssetId(importEnvironmentId_.data()) &&
                               !importDraft_->profileIds.empty();
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Import") && actions.importEnvironment) {
                actions.importEnvironment(
                    {importDraft_->source, importDisplayName_.data(),
                     importEnvironmentId_.data(),
                     importDraft_->profileIds[importProfileIndex_]});
                selectedEnvironmentId_ = importEnvironmentId_.data();
                importDraft_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                importDraft_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (!snapshot.environmentStatus.empty())
        editor::statusIndicator(snapshot.environmentStatus.c_str(),
                                editor::StatusTone::Info);
    if (!snapshot.environmentError.empty())
        editor::statusIndicator(snapshot.environmentError.c_str(),
                                editor::StatusTone::Error);

    }

    if (!environmentsOnly) {
    ImGui::SeparatorText("Current Import");
    if (snapshot.activeTask) {
        const AssetTaskSnapshot &task = *snapshot.activeTask;
        ImGui::Text("Task: %llu  %s",
                    static_cast<unsigned long long>(task.id),
                    task.state.c_str());
        ImGui::Text("%s: %s / %s", task.kind.c_str(), task.assetId.c_str(),
                    task.profileId.c_str());
        const float progress = task.total == 0
                                   ? 0.0f
                                   : static_cast<float>(task.completed) /
                                         static_cast<float>(task.total);
        ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f));
        ImGui::Text("Artifacts: %llu/%llu  encoded %llu  reused %llu  failed %llu",
                    static_cast<unsigned long long>(task.completed),
                    static_cast<unsigned long long>(task.total),
                    static_cast<unsigned long long>(task.encoded),
                    static_cast<unsigned long long>(task.reused),
                    static_cast<unsigned long long>(task.failed));
        ImGui::Text("Workers: %u  elapsed: %.1f s", task.workers,
                    task.elapsedSeconds);
        ImGui::BeginDisabled(task.terminal);
        if (ImGui::Button("Cancel Asset Import") && actions.cancelTask)
            actions.cancelTask(task.id);
        ImGui::EndDisabled();
        if (!task.logPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open Log") && actions.openPath)
                actions.openPath(task.logPath);
        }
    } else {
        ImGui::TextUnformatted(snapshot.authoringCompiled
                                   ? "Idle"
                                   : "Asset authoring is not compiled.");
    }

    if (!snapshot.recentTasks.empty()) {
        ImGui::SeparatorText("Recent Imports");
        for (const AssetTaskSnapshot &task : snapshot.recentTasks) {
            ImGui::PushID(static_cast<int>(task.id));
            if (ImGui::TreeNode("Import", "%s: %s / %s [%s]##%llu",
                                task.kind.c_str(), task.assetId.c_str(),
                                task.profileId.c_str(), task.state.c_str(),
                                static_cast<unsigned long long>(task.id))) {
                ImGui::Text("Encoded: %llu  Reused: %llu  Failed: %llu",
                            static_cast<unsigned long long>(task.encoded),
                            static_cast<unsigned long long>(task.reused),
                            static_cast<unsigned long long>(task.failed));
                if (!task.error.empty())
                    ImGui::TextWrapped("Error: %s", task.error.c_str());
                if (!task.logPath.empty() && ImGui::Button("Open Log") &&
                    actions.openPath)
                    actions.openPath(task.logPath);
                if (!task.reportPath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Open Report") && actions.openPath)
                        actions.openPath(task.reportPath);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    }
}

} // namespace vkr
