#include "InspectorPanel.h"

#include "editor/EditorWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vkr {
namespace {

glm::vec3 quaternionToEulerDegrees(const glm::quat &input) {
    const glm::quat q = glm::normalize(input);
    const float sinX = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.0f * (q.w * q.y - q.z * q.x),
                                  -1.0f, 1.0f);
    const float sinZ = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    return glm::degrees(glm::vec3(std::atan2(sinX, cosX), std::asin(sinY),
                                  std::atan2(sinZ, cosZ)));
}

glm::quat eulerDegreesToQuaternion(const glm::vec3 &degrees) {
    const glm::vec3 half = glm::radians(degrees) * 0.5f;
    const glm::quat qx(std::cos(half.x), std::sin(half.x), 0.0f, 0.0f);
    const glm::quat qy(std::cos(half.y), 0.0f, std::sin(half.y), 0.0f);
    const glm::quat qz(std::cos(half.z), 0.0f, 0.0f, std::sin(half.z));
    return glm::normalize(qz * qy * qx);
}

void beginContinuousIfNeeded(bool changed, bool &active,
                             const char *label,
                             const InspectorPanelActions &actions) {
    if (changed && !active) {
        if (actions.beginContinuous)
            actions.beginContinuous(label);
        active = true;
    }
}

void endContinuousIfNeeded(bool &active,
                           const InspectorPanelActions &actions) {
    if (active && ImGui::IsItemDeactivatedAfterEdit()) {
        if (actions.commitContinuous)
            actions.commitContinuous();
        active = false;
    }
}

bool isDescendantOf(const InspectorPanelSnapshot &snapshot,
                    const PersistentEntityId &candidate,
                    const PersistentEntityId &ancestor) {
    const RuntimeEntitySnapshot *current = nullptr;
    for (const RuntimeEntitySnapshot &entity : snapshot.entities) {
        if (entity.id == candidate) {
            current = &entity;
            break;
        }
    }
    while (current && current->parent) {
        if (*current->parent == ancestor)
            return true;
        const PersistentEntityId parent = *current->parent;
        current = nullptr;
        for (const RuntimeEntitySnapshot &entity : snapshot.entities) {
            if (entity.id == parent) {
                current = &entity;
                break;
            }
        }
    }
    return false;
}

} // namespace

void InspectorPanel::draw(const InspectorPanelSnapshot &snapshot,
                          const InspectorPanelActions &actions) {
    if (!ImGui::BeginTabBar("InspectorTabs"))
        return;

    if (ImGui::BeginTabItem("Entity")) {
        if (!snapshot.entity) {
            ImGui::TextDisabled("Select an entity in the Outliner.");
        } else {
            const RuntimeEntitySnapshot &entity = *snapshot.entity;
            ImGui::BeginDisabled(!snapshot.editable);
            char name[192]{};
            std::snprintf(name, sizeof(name), "%s", entity.name.c_str());
            if (ImGui::InputText("Name", name, sizeof(name),
                                 ImGuiInputTextFlags_EnterReturnsTrue) &&
                actions.setName) {
                actions.setName(entity.id, name);
            }
            bool enabled = entity.enabled;
            if (ImGui::Checkbox("Enabled", &enabled) && actions.setEnabled)
                actions.setEnabled(entity.id, enabled);

            const char *parentName = "None";
            if (entity.parent) {
                for (const RuntimeEntitySnapshot &candidate :
                     snapshot.entities) {
                    if (candidate.id == *entity.parent) {
                        parentName = candidate.name.c_str();
                        break;
                    }
                }
            }
            if (ImGui::BeginCombo("Parent", parentName)) {
                const bool rootSelected = !entity.parent;
                if (ImGui::Selectable("None", rootSelected) &&
                    actions.setParent) {
                    actions.setParent(entity.id, std::nullopt);
                }
                for (const RuntimeEntitySnapshot &candidate :
                     snapshot.entities) {
                    if (candidate.id == entity.id ||
                        isDescendantOf(snapshot, candidate.id, entity.id)) {
                        continue;
                    }
                    const bool selected = entity.parent &&
                                          *entity.parent == candidate.id;
                    if (ImGui::Selectable(candidate.name.c_str(), selected) &&
                        actions.setParent) {
                        actions.setParent(entity.id, candidate.id);
                    }
                }
                ImGui::EndCombo();
            }

            if (ImGui::CollapsingHeader("Transform",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                SceneTransformDocument transform = entity.transform;
                bool changed = ImGui::DragFloat3(
                    "Translation", &transform.translation.x, 0.05f);
                beginContinuousIfNeeded(changed, transformEditing_,
                                        "Edit Transform", actions);
                if (changed && actions.setTransform)
                    actions.setTransform(entity.id, transform);
                endContinuousIfNeeded(transformEditing_, actions);

                glm::vec3 euler =
                    quaternionToEulerDegrees(entity.transform.rotation);
                changed = ImGui::DragFloat3("Rotation", &euler.x, 0.5f);
                beginContinuousIfNeeded(changed, transformEditing_,
                                        "Edit Transform", actions);
                if (changed && actions.setTransform) {
                    transform = entity.transform;
                    transform.rotation = eulerDegreesToQuaternion(euler);
                    actions.setTransform(entity.id, transform);
                }
                endContinuousIfNeeded(transformEditing_, actions);

                transform = entity.transform;
                changed = ImGui::DragFloat3("Scale", &transform.scale.x,
                                            0.01f, -1000.0f, 1000.0f);
                beginContinuousIfNeeded(changed, transformEditing_,
                                        "Edit Transform", actions);
                if (changed && actions.setTransform) {
                    const float epsilon = 0.0001f;
                    for (int axis = 0; axis < 3; ++axis) {
                        if (std::abs(transform.scale[axis]) < epsilon)
                            transform.scale[axis] =
                                transform.scale[axis] < 0.0f ? -epsilon
                                                             : epsilon;
                    }
                    actions.setTransform(entity.id, transform);
                }
                endContinuousIfNeeded(transformEditing_, actions);
            }

            if (ImGui::CollapsingHeader("Model",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                const char *current = "None";
                if (entity.modelInstance) {
                    current = entity.modelInstance->model.value().c_str();
                    for (const InspectorModelOption &model : snapshot.models) {
                        if (model.id == entity.modelInstance->model.value()) {
                            current = model.displayName.c_str();
                            break;
                        }
                    }
                }
                if (ImGui::BeginCombo("Model Asset", current)) {
                    if (ImGui::Selectable("None", !entity.modelInstance) &&
                        actions.setModel) {
                        actions.setModel(entity.id, std::nullopt);
                    }
                    for (const InspectorModelOption &model : snapshot.models) {
                        ImGui::BeginDisabled(!model.instanceable);
                        const bool selected = entity.modelInstance &&
                            entity.modelInstance->model.value() == model.id;
                        if (ImGui::Selectable(model.displayName.c_str(),
                                              selected) &&
                            actions.setModel) {
                            actions.setModel(
                                entity.id,
                                ModelInstanceDocument{ModelAssetId(model.id)});
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                if (entity.modelInstance) {
                    ImGui::TextDisabled("Binding: %s",
                                        modelBindingStateName(
                                            entity.modelBindingState));
                    if (!entity.modelBindingError.empty())
                        ImGui::TextWrapped("%s",
                                           entity.modelBindingError.c_str());
                }
            }

            if (ImGui::CollapsingHeader("Light")) {
                bool present = entity.light.has_value();
                if (ImGui::Checkbox("Light Component", &present) &&
                    actions.setLight) {
                    actions.setLight(
                        entity.id,
                        present ? std::optional<LightComponentDocument>(
                                      LightComponentDocument{})
                                : std::nullopt);
                }
                if (entity.light) {
                    if (snapshot.selectedLightLimitExceeded) {
                        ImGui::TextColored(
                            editor::statusColor(editor::StatusTone::Warning),
                            "Not uploaded: GPU light limit reached");
                    }
                    LightComponentDocument light = *entity.light;
                    int type = static_cast<int>(light.type);
                    const char *types[] = {"Directional", "Point", "Spot"};
                    if (ImGui::Combo("Type", &type, types, 3) &&
                        actions.setLight) {
                        light.type = static_cast<SceneDocumentLightType>(type);
                        actions.setLight(entity.id, light);
                    }
                    bool changed = ImGui::ColorEdit3("Color", &light.color.x);
                    beginContinuousIfNeeded(changed, lightEditing_,
                                            "Edit Light", actions);
                    if (changed && actions.setLight)
                        actions.setLight(entity.id, light);
                    endContinuousIfNeeded(lightEditing_, actions);
                    changed = ImGui::DragFloat("Intensity", &light.intensity,
                                               0.05f, 0.0f, 100000.0f);
                    beginContinuousIfNeeded(changed, lightEditing_,
                                            "Edit Light", actions);
                    if (changed && actions.setLight)
                        actions.setLight(entity.id, light);
                    endContinuousIfNeeded(lightEditing_, actions);
                    if (light.type != SceneDocumentLightType::Directional) {
                        float range = light.range.value_or(10.0f);
                        changed = ImGui::DragFloat("Range", &range, 0.1f,
                                                   0.001f, 100000.0f);
                        beginContinuousIfNeeded(changed, lightEditing_,
                                                "Edit Light", actions);
                        if (changed && actions.setLight) {
                            light.range = range;
                            actions.setLight(entity.id, light);
                        }
                        endContinuousIfNeeded(lightEditing_, actions);
                    }
                    if (light.type == SceneDocumentLightType::Spot) {
                        float inner = glm::degrees(light.innerConeRadians);
                        float outer = glm::degrees(light.outerConeRadians);
                        changed = ImGui::DragFloat("Inner Cone", &inner,
                                                   0.25f, 0.0f, 89.0f);
                        changed |= ImGui::DragFloat("Outer Cone", &outer,
                                                    0.25f, 0.1f, 89.9f);
                        beginContinuousIfNeeded(changed, lightEditing_,
                                                "Edit Light", actions);
                        if (changed && actions.setLight) {
                            outer = std::max(outer, 0.1f);
                            inner = std::min(inner, outer);
                            light.innerConeRadians = glm::radians(inner);
                            light.outerConeRadians = glm::radians(outer);
                            actions.setLight(entity.id, light);
                        }
                        endContinuousIfNeeded(lightEditing_, actions);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Camera")) {
                bool present = entity.camera.has_value();
                if (ImGui::Checkbox("Camera Component", &present) &&
                    actions.setCamera) {
                    actions.setCamera(
                        entity.id,
                        present ? std::optional<CameraComponentDocument>(
                                      CameraComponentDocument{})
                                : std::nullopt);
                }
                if (entity.camera) {
                    CameraComponentDocument camera = *entity.camera;
                    float fov = glm::degrees(camera.verticalFovRadians);
                    bool changed = ImGui::DragFloat("Vertical FOV", &fov,
                                                    0.25f, 1.0f, 179.0f);
                    changed |= ImGui::DragFloat("Near", &camera.nearPlane,
                                                0.001f, 0.001f, 1000.0f);
                    changed |= ImGui::DragFloat("Far", &camera.farPlane,
                                                1.0f, 0.01f, 1000000.0f);
                    beginContinuousIfNeeded(changed, cameraEditing_,
                                            "Edit Camera", actions);
                    if (changed && actions.setCamera) {
                        camera.verticalFovRadians = glm::radians(fov);
                        camera.farPlane =
                            std::max(camera.farPlane,
                                     camera.nearPlane + 0.001f);
                        actions.setCamera(entity.id, camera);
                    }
                    endContinuousIfNeeded(cameraEditing_, actions);
                    const bool active = snapshot.activeCamera &&
                                        *snapshot.activeCamera == entity.id;
                    ImGui::BeginDisabled(active);
                    if (ImGui::Button(active ? "Active Camera"
                                             : "Set Active") &&
                        actions.setActiveCamera) {
                        actions.setActiveCamera(entity.id);
                    }
                    ImGui::EndDisabled();
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Scene")) {
        ImGui::Text("%s", snapshot.sceneDisplayName.c_str());
        ImGui::TextDisabled("%s", snapshot.sceneId.value().c_str());
        ImGui::BeginDisabled(!snapshot.editable);
        SceneAmbientDocument ambient = snapshot.ambient;
        bool changed = ImGui::ColorEdit3("Ambient Color", &ambient.color.x);
        changed |= ImGui::DragFloat("Ambient Intensity", &ambient.intensity,
                                    0.01f, 0.0f, 100.0f);
        beginContinuousIfNeeded(changed, sceneEditing_, "Edit Ambient",
                                actions);
        if (changed && actions.setAmbient)
            actions.setAmbient(ambient);
        endContinuousIfNeeded(sceneEditing_, actions);

        const char *environmentName = "None";
        if (snapshot.environment) {
            environmentName = snapshot.environment->environmentId.c_str();
            for (const InspectorEnvironmentOption &environment :
                 snapshot.environments) {
                if (environment.id ==
                    snapshot.environment->environmentId) {
                    environmentName = environment.displayName.c_str();
                    break;
                }
            }
        }
        if (ImGui::BeginCombo("Environment", environmentName)) {
            if (ImGui::Selectable("None", !snapshot.environment) &&
                actions.setEnvironment) {
                actions.setEnvironment(std::nullopt);
            }
            for (const InspectorEnvironmentOption &environment :
                 snapshot.environments) {
                const bool selected = snapshot.environment &&
                    snapshot.environment->environmentId == environment.id;
                if (ImGui::Selectable(environment.displayName.c_str(),
                                      selected) &&
                    actions.setEnvironment) {
                    actions.setEnvironment(SceneEnvironmentDocument{
                        environment.id, 1.0f, 0.0f});
                }
            }
            ImGui::EndCombo();
        }
        if (snapshot.environment) {
            SceneEnvironmentDocument environment = *snapshot.environment;
            changed = ImGui::DragFloat("Environment Intensity",
                                       &environment.intensity, 0.01f, 0.0f,
                                       100.0f);
            float degrees = glm::degrees(environment.rotationRadians);
            changed |= ImGui::DragFloat("Environment Rotation", &degrees,
                                        0.5f, -360.0f, 360.0f);
            beginContinuousIfNeeded(changed, sceneEditing_,
                                    "Edit Environment", actions);
            if (changed && actions.setEnvironment) {
                environment.rotationRadians = glm::radians(degrees);
                actions.setEnvironment(environment);
            }
            endContinuousIfNeeded(sceneEditing_, actions);
        }

        const char *activeCameraName = "None";
        for (const RuntimeEntitySnapshot &camera : snapshot.cameraEntities) {
            if (snapshot.activeCamera && camera.id == *snapshot.activeCamera)
                activeCameraName = camera.name.c_str();
        }
        if (ImGui::BeginCombo("Active Camera", activeCameraName)) {
            for (const RuntimeEntitySnapshot &camera : snapshot.cameraEntities) {
                const bool selected = snapshot.activeCamera &&
                                      camera.id == *snapshot.activeCamera;
                if (ImGui::Selectable(camera.name.c_str(), selected) &&
                    actions.setActiveCamera) {
                    actions.setActiveCamera(camera.id);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace vkr
