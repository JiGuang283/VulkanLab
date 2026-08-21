#include "InspectorPanel.h"

#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
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

const char *entityIcon(const RuntimeEntitySnapshot &entity) {
    if (entity.atmosphere)
        return icons::Environment;
    if (entity.camera)
        return icons::Camera;
    if (entity.light)
        return entity.light->atmosphereSunIndex ? icons::Sun : icons::Light;
    if (entity.reflectionProbe)
        return icons::Image;
    if (entity.ddgiProbeVolume)
        return icons::Grid;
    if (entity.modelInstance)
        return icons::Model;
    return icons::Box;
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
            if (editor::iconsAvailable()) {
                ImGui::Text("%s", entityIcon(entity));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(entity.name.c_str());
            ImGui::SameLine();
            if (editor::iconButton("CopyUuid", icons::Copy, "C",
                                   "Copy entity UUID")) {
                const std::string id = entity.id.toString();
                ImGui::SetClipboardText(id.c_str());
            }
            ImGui::TextDisabled("%s", entity.id.toString().c_str());
            ImGui::Separator();
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

            if (editor::iconButton("AddComponent", icons::Plus, "+",
                                   "Add component"))
                ImGui::OpenPopup("Add Component");
            ImGui::SameLine();
            ImGui::TextUnformatted("Add Component");
            if (ImGui::BeginPopup("Add Component")) {
                if (!entity.modelInstance && ImGui::BeginMenu("Model")) {
                    for (const InspectorModelOption &model : snapshot.models) {
                        ImGui::BeginDisabled(!model.instanceable);
                        if (ImGui::MenuItem(model.displayName.c_str()) &&
                            actions.setModel) {
                            actions.setModel(
                                entity.id,
                                ModelInstanceDocument{ModelAssetId(model.id)});
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndMenu();
                }
                if (!entity.light && ImGui::MenuItem("Light") &&
                    actions.setLight) {
                    actions.setLight(entity.id, LightComponentDocument{});
                }
                if (!entity.camera && ImGui::MenuItem("Camera") &&
                    actions.setCamera) {
                    actions.setCamera(entity.id, CameraComponentDocument{});
                }
                ImGui::BeginDisabled(snapshot.atmospherePresent ||
                                     entity.parent.has_value());
                if (!entity.atmosphere &&
                    ImGui::MenuItem("Sky Atmosphere") &&
                    actions.setAtmosphere) {
                    actions.setAtmosphere(
                        entity.id, AtmosphereComponentDocument{});
                }
                ImGui::EndDisabled();
                if (!entity.reflectionProbe &&
                    ImGui::MenuItem("Reflection Probe") &&
                    actions.setReflectionProbe) {
                    actions.setReflectionProbe(
                        entity.id, ReflectionProbeComponentDocument{});
                }
                ImGui::BeginDisabled(entity.parent.has_value());
                if (!entity.ddgiProbeVolume &&
                    ImGui::MenuItem("DDGI Probe Volume") &&
                    actions.setDdgiProbeVolume) {
                    actions.setDdgiProbeVolume(
                        entity.id, DdgiProbeVolumeComponentDocument{});
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }

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
            ImGui::BeginDisabled(entity.atmosphere.has_value());
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
            ImGui::EndDisabled();

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

                ImGui::BeginDisabled(entity.atmosphere.has_value());
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
                ImGui::EndDisabled();
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
                    ImGui::TextDisabled("Entity enabled: %s",
                                        entity.enabled ? "Yes" : "No");
                    ImGui::TextDisabled("Effective enabled: %s",
                                        entity.effectiveEnabled ? "Yes"
                                                                : "No");
                    if (snapshot.selectedLightUploadStatus ==
                        InspectorLightUploadStatus::NotUploaded) {
                        ImGui::TextColored(
                            editor::statusColor(editor::StatusTone::Warning),
                            "Not uploaded: GPU light limit reached");
                    } else if (snapshot.selectedLightUploadStatus ==
                               InspectorLightUploadStatus::Ineffective) {
                        ImGui::TextDisabled("Upload: Ineffective");
                    } else {
                        ImGui::TextDisabled("Upload: Active");
                    }
                    const char *shadowStatus = "Disabled";
                    switch (snapshot.selectedLightShadowStatus) {
                    case InspectorLightShadowStatus::Active:
                        shadowStatus = "Active";
                        break;
                    case InspectorLightShadowStatus::Eligible:
                        shadowStatus = "Eligible";
                        break;
                    case InspectorLightShadowStatus::BudgetExceeded:
                        shadowStatus = "Budget Exceeded";
                        break;
                    case InspectorLightShadowStatus::Disabled:
                        shadowStatus = "Disabled";
                        break;
                    case InspectorLightShadowStatus::Unsupported:
                        shadowStatus = "Unsupported";
                        break;
                    }
                    ImGui::TextDisabled("Shadow: %s", shadowStatus);
                    LightComponentDocument light = *entity.light;
                    int type = static_cast<int>(light.type);
                    const char *types[] = {"Directional", "Point", "Spot"};
                    if (ImGui::Combo("Type", &type, types, 3) &&
                        actions.setLight) {
                        light.type = static_cast<SceneDocumentLightType>(type);
                        if (light.type !=
                            SceneDocumentLightType::Directional) {
                            light.atmosphereSunIndex.reset();
                        }
                        actions.setLight(entity.id, light);
                    }
                    {
                        ImGui::TextDisabled(
                            "Policy: %s",
                            light.castsShadow ? "Forced" : "Disabled");
                        bool castsShadow = light.castsShadow;
                        if (ImGui::Checkbox("Casts Shadow", &castsShadow) &&
                            actions.setLight) {
                            light.castsShadow = castsShadow;
                            actions.setLight(entity.id, light);
                        }
                    }
                    if (light.type == SceneDocumentLightType::Directional) {
                        bool atmosphereSun =
                            light.atmosphereSunIndex == 0u;
                        ImGui::BeginDisabled(!snapshot.atmospherePresent);
                        if (ImGui::Checkbox("Use as Atmosphere Sun",
                                            &atmosphereSun) &&
                            actions.setAtmosphereSun) {
                            actions.setAtmosphereSun(entity.id,
                                                     atmosphereSun);
                        }
                        ImGui::EndDisabled();
                        if (atmosphereSun) {
                            float angularRadiusDegrees = glm::degrees(
                                light.sourceAngularRadiusRadians);
                            bool changed = ImGui::DragFloat(
                                "Sun Angular Radius", &angularRadiusDegrees,
                                0.005f, 0.01f, 5.0f, "%.3f deg");
                            beginContinuousIfNeeded(
                                changed, lightEditing_, "Edit Light",
                                actions);
                            if (changed && actions.setLight) {
                                light.sourceAngularRadiusRadians =
                                    glm::radians(angularRadiusDegrees);
                                actions.setLight(entity.id, light);
                            }
                            endContinuousIfNeeded(lightEditing_, actions);
                        }
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

            if (entity.atmosphere &&
                ImGui::CollapsingHeader("Sky Atmosphere",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                AtmosphereComponentDocument atmosphere = *entity.atmosphere;
                if (ImGui::Button("Earth Preset") &&
                    actions.setAtmosphere) {
                    actions.setAtmosphere(entity.id,
                                          AtmosphereComponentDocument{});
                }
                const auto applyAtmosphereEdit = [&](bool changed) {
                    beginContinuousIfNeeded(changed, atmosphereEditing_,
                                            "Edit Sky Atmosphere", actions);
                    if (changed && actions.setAtmosphere) {
                        atmosphere.mieExtinctionPerKm = std::max(
                            atmosphere.mieExtinctionPerKm,
                            atmosphere.mieScatteringPerKm);
                        actions.setAtmosphere(entity.id, atmosphere);
                    }
                    endContinuousIfNeeded(atmosphereEditing_, actions);
                };
                bool changed = ImGui::DragFloat(
                    "Ground Radius (km)", &atmosphere.bottomRadiusKm, 1.0f,
                    100.0f, 100000.0f);
                applyAtmosphereEdit(changed);
                changed = ImGui::DragFloat(
                    "Atmosphere Height (km)",
                    &atmosphere.atmosphereHeightKm, 0.1f, 1.0f, 1000.0f);
                applyAtmosphereEdit(changed);
                changed = ImGui::ColorEdit3("Ground Albedo",
                                             &atmosphere.groundAlbedo.x);
                applyAtmosphereEdit(changed);
                changed = ImGui::DragFloat(
                    "Multiple Scattering",
                    &atmosphere.multipleScatteringFactor, 0.01f, 0.0f,
                    4.0f);
                applyAtmosphereEdit(changed);
                changed = ImGui::DragFloat(
                    "Aerial Start (m)",
                    &atmosphere.aerialPerspectiveStartMeters, 1.0f, 0.0f,
                    100000.0f);
                applyAtmosphereEdit(changed);
                changed = ImGui::DragFloat(
                    "Aerial Distance Scale",
                    &atmosphere.aerialPerspectiveDistanceScale, 0.01f,
                    0.01f, 10.0f);
                applyAtmosphereEdit(changed);

                if (ImGui::TreeNodeEx("Advanced Scattering",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
                    changed = ImGui::DragFloat3(
                        "Rayleigh / km",
                        &atmosphere.rayleighScatteringPerKm.x, 0.00001f,
                        0.0f, 1.0f, "%.6f");
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Rayleigh Height (km)",
                        &atmosphere.rayleighScaleHeightKm, 0.01f, 0.01f,
                        100.0f);
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Mie Scattering / km",
                        &atmosphere.mieScatteringPerKm, 0.00001f, 0.0f,
                        1.0f, "%.6f");
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Mie Extinction / km",
                        &atmosphere.mieExtinctionPerKm, 0.00001f, 0.0f,
                        1.0f, "%.6f");
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Mie Height (km)", &atmosphere.mieScaleHeightKm,
                        0.01f, 0.01f, 100.0f);
                    applyAtmosphereEdit(changed);
                    changed = ImGui::SliderFloat(
                        "Mie Anisotropy", &atmosphere.mieAnisotropy,
                        -0.99f, 0.99f);
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat3(
                        "Ozone Absorption / km",
                        &atmosphere.ozoneAbsorptionPerKm.x, 0.00001f, 0.0f,
                        1.0f, "%.6f");
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Ozone Center (km)",
                        &atmosphere.ozoneCenterHeightKm, 0.1f, 0.0f,
                        atmosphere.atmosphereHeightKm);
                    applyAtmosphereEdit(changed);
                    changed = ImGui::DragFloat(
                        "Ozone Half Width (km)",
                        &atmosphere.ozoneHalfWidthKm, 0.1f, 0.01f,
                        atmosphere.atmosphereHeightKm);
                    applyAtmosphereEdit(changed);
                    ImGui::TreePop();
                }
            }

            if (entity.reflectionProbe &&
                ImGui::CollapsingHeader("Reflection Probe",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ReflectionProbeComponentDocument probe =
                    *entity.reflectionProbe;
                const auto applyProbeEdit = [&](bool changed) {
                    beginContinuousIfNeeded(
                        changed, reflectionProbeEditing_,
                        "Edit Reflection Probe", actions);
                    if (changed && actions.setReflectionProbe)
                        actions.setReflectionProbe(entity.id, probe);
                    endContinuousIfNeeded(reflectionProbeEditing_, actions);
                };

                const char *environmentName = "Not Captured";
                if (probe.environmentId) {
                    environmentName = probe.environmentId->c_str();
                    for (const InspectorEnvironmentOption &environment :
                         snapshot.environments) {
                        if (environment.id == *probe.environmentId) {
                            environmentName = environment.displayName.c_str();
                            break;
                        }
                    }
                }
                if (ImGui::BeginCombo("Environment", environmentName)) {
                    if (ImGui::Selectable("Not Captured",
                                          !probe.environmentId) &&
                        actions.setReflectionProbe) {
                        probe.environmentId.reset();
                        actions.setReflectionProbe(entity.id, probe);
                    }
                    for (const InspectorEnvironmentOption &environment :
                         snapshot.environments) {
                        const bool selected = probe.environmentId &&
                                              *probe.environmentId ==
                                                  environment.id;
                        if (ImGui::Selectable(environment.displayName.c_str(),
                                              selected) &&
                            actions.setReflectionProbe) {
                            probe.environmentId = environment.id;
                            actions.setReflectionProbe(entity.id, probe);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled(
                    "Binding: %s",
                    modelBindingStateName(
                        entity.reflectionProbeBindingState));
                if (!entity.reflectionProbeBindingError.empty())
                    ImGui::TextWrapped(
                        "%s", entity.reflectionProbeBindingError.c_str());

                int shape = probe.shape == ReflectionProbeShape::Box ? 0 : 1;
                if (ImGui::Combo("Shape", &shape, "Box\0Sphere\0") &&
                    actions.setReflectionProbe) {
                    probe.shape = shape == 0 ? ReflectionProbeShape::Box
                                             : ReflectionProbeShape::Sphere;
                    actions.setReflectionProbe(entity.id, probe);
                }
                bool changed = false;
                if (probe.shape == ReflectionProbeShape::Box) {
                    changed = ImGui::DragFloat3(
                        "Box Extents", &probe.boxExtents.x, 0.05f,
                        0.01f, 100000.0f);
                    probe.boxExtents = glm::max(
                        probe.boxExtents, glm::vec3(0.01f));
                    applyProbeEdit(changed);
                } else {
                    changed = ImGui::DragFloat(
                        "Sphere Radius", &probe.sphereRadius, 0.05f,
                        0.01f, 100000.0f);
                    probe.sphereRadius = std::max(probe.sphereRadius, 0.01f);
                    applyProbeEdit(changed);
                }
                changed = ImGui::DragFloat(
                    "Blend Distance", &probe.blendDistance, 0.05f,
                    0.0f, 100000.0f);
                probe.blendDistance = std::max(probe.blendDistance, 0.0f);
                applyProbeEdit(changed);
                changed = ImGui::DragInt("Priority", &probe.priority, 1.0f);
                applyProbeEdit(changed);
                changed = ImGui::DragFloat(
                    "Intensity", &probe.intensity, 0.01f, 0.0f, 100.0f);
                probe.intensity = std::max(probe.intensity, 0.0f);
                applyProbeEdit(changed);
                changed = ImGui::Checkbox("Box Projection",
                                          &probe.boxProjection);
                applyProbeEdit(changed);
                changed = ImGui::DragFloat3(
                    "Capture Offset", &probe.captureOffset.x, 0.05f);
                applyProbeEdit(changed);

                if (snapshot.reflectionProbeCaptureActive)
                    ImGui::TextDisabled(
                        "%s", snapshot.reflectionProbeCaptureStatus.c_str());
                ImGui::BeginDisabled(
                    !snapshot.reflectionProbeCaptureAvailable ||
                    snapshot.reflectionProbeCaptureActive ||
                    !actions.captureReflectionProbe);
                if (ImGui::Button("Capture and Bake") &&
                    actions.captureReflectionProbe) {
                    actions.captureReflectionProbe(entity.id);
                }
                ImGui::EndDisabled();
            }

            if (entity.ddgiProbeVolume &&
                ImGui::CollapsingHeader("DDGI Probe Volume",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                DdgiProbeVolumeComponentDocument volume =
                    *entity.ddgiProbeVolume;
                const auto applyVolumeEdit = [&](bool changed) {
                    beginContinuousIfNeeded(
                        changed, ddgiProbeVolumeEditing_,
                        "Edit DDGI Probe Volume", actions);
                    if (changed && actions.setDdgiProbeVolume)
                        actions.setDdgiProbeVolume(entity.id, volume);
                    endContinuousIfNeeded(ddgiProbeVolumeEditing_, actions);
                };

                int counts[3] = {
                    static_cast<int>(volume.probeCounts.x),
                    static_cast<int>(volume.probeCounts.y),
                    static_cast<int>(volume.probeCounts.z)};
                bool changed = ImGui::InputInt3("Probe Counts", counts);
                if (changed) {
                    for (int &count : counts)
                        count = std::clamp(count, 1, 32);
                    while (static_cast<uint64_t>(counts[0]) * counts[1] *
                               counts[2] >
                           2048) {
                        const int largest =
                            counts[0] >= counts[1]
                                ? (counts[0] >= counts[2] ? 0 : 2)
                                : (counts[1] >= counts[2] ? 1 : 2);
                        --counts[largest];
                    }
                    volume.probeCounts = glm::uvec3(
                        counts[0], counts[1], counts[2]);
                    volume.probesUpdatedPerFrame = std::min(
                        volume.probesUpdatedPerFrame,
                        volume.probeCounts.x * volume.probeCounts.y *
                            volume.probeCounts.z);
                }
                applyVolumeEdit(changed);
                changed = ImGui::DragFloat3(
                    "Probe Spacing", &volume.probeSpacing.x, 0.05f,
                    0.05f, 1000.0f);
                volume.probeSpacing =
                    glm::max(volume.probeSpacing, glm::vec3(0.05f));
                applyVolumeEdit(changed);

                int rays = volume.raysPerProbe == 64
                               ? 0
                               : (volume.raysPerProbe == 128 ? 1 : 2);
                if (ImGui::Combo("Rays / Probe", &rays,
                                 "64\0 128\0 256\0")) {
                    volume.raysPerProbe = rays == 0 ? 64u
                                                    : (rays == 1 ? 128u
                                                                 : 256u);
                    if (actions.setDdgiProbeVolume)
                        actions.setDdgiProbeVolume(entity.id, volume);
                }
                int updateCount = static_cast<int>(
                    volume.probesUpdatedPerFrame);
                const int totalProbes = static_cast<int>(
                    volume.probeCounts.x * volume.probeCounts.y *
                    volume.probeCounts.z);
                changed = ImGui::SliderInt("Update Budget", &updateCount, 1,
                                           std::max(totalProbes, 1));
                volume.probesUpdatedPerFrame =
                    static_cast<uint32_t>(updateCount);
                applyVolumeEdit(changed);
                changed = ImGui::DragFloat("Max Ray Distance",
                                           &volume.maxRayDistance, 0.1f,
                                           0.1f, 10000.0f);
                applyVolumeEdit(changed);
                changed = ImGui::SliderFloat("Hysteresis",
                                             &volume.hysteresis, 0.0f,
                                             0.999f, "%.3f");
                applyVolumeEdit(changed);
                changed = ImGui::DragFloat("Normal Bias",
                                           &volume.normalBias, 0.01f,
                                           0.0f, 10.0f);
                applyVolumeEdit(changed);
                changed = ImGui::DragFloat("View Bias", &volume.viewBias,
                                           0.01f, 0.0f, 10.0f);
                applyVolumeEdit(changed);
                changed = ImGui::SliderFloat("Intensity", &volume.intensity,
                                             0.0f, 4.0f);
                applyVolumeEdit(changed);
                changed = ImGui::Checkbox("Relocation",
                                          &volume.relocationEnabled);
                applyVolumeEdit(changed);
                changed = ImGui::Checkbox("Classification",
                                          &volume.classificationEnabled);
                applyVolumeEdit(changed);
                ImGui::TextDisabled("Probe count: %u / 2048",
                                    volume.probeCounts.x *
                                        volume.probeCounts.y *
                                        volume.probeCounts.z);
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
