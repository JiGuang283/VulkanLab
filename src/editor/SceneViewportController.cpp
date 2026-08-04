#include "SceneViewportController.h"

#include "EditorDragDrop.h"
#include "EditorWidgets.h"
#include "SceneEditorSession.h"
#include "scene/ModelAsset.h"
#include "scene/BoundsMath.h"
#include "scene/TransformMath.h"

#include <ImGuizmo.h>
#include <imgui.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vkr {
namespace {

constexpr ImU32 kSelectionColor = IM_COL32(255, 176, 54, 255);
constexpr ImU32 kPlacementColor = IM_COL32(76, 190, 255, 255);

ImGuizmo::OPERATION imGuizmoOperation(GizmoOperation operation) {
    switch (operation) {
    case GizmoOperation::Translate:
        return ImGuizmo::TRANSLATE;
    case GizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case GizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case GizmoOperation::Select:
        break;
    }
    return ImGuizmo::TRANSLATE;
}

bool finiteVector(const glm::vec3 &value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

std::optional<ImVec2> projectPoint(const glm::vec3 &point,
                                   const glm::mat4 &viewProjection,
                                   const EditorViewportState &viewport) {
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
    if (!std::isfinite(clip.w) || clip.w <= 1.0e-5f)
        return std::nullopt;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return ImVec2(
        viewport.minX + (ndc.x * 0.5f + 0.5f) * viewport.logicalWidth,
        viewport.minY + (ndc.y * 0.5f + 0.5f) * viewport.logicalHeight);
}

std::optional<glm::vec3>
placementPoint(const SceneViewportController::Ray &ray,
               const SceneViewportCamera &camera,
               const RuntimeWorld &world,
               const std::optional<PersistentEntityId> &selection) {
    if (std::abs(ray.direction.z) > 1.0e-6f) {
        const float distance = -ray.origin.z / ray.direction.z;
        if (distance > 0.0f)
            return ray.origin + ray.direction * distance;
    }

    glm::vec3 planePoint{0.0f};
    if (selection) {
        if (const auto selected = world.entity(world.find(*selection)))
            planePoint = glm::vec3(selected->world[3]);
    } else if (world.bounds().valid) {
        planePoint = world.bounds().center;
    }
    const glm::vec3 normal = glm::normalize(camera.forward);
    const float denominator = glm::dot(ray.direction, normal);
    if (std::abs(denominator) <= 1.0e-6f)
        return std::nullopt;
    const float distance = glm::dot(planePoint - ray.origin, normal) /
                           denominator;
    if (distance <= 0.0f)
        return std::nullopt;
    return ray.origin + ray.direction * distance;
}

void drawSelection(const RuntimeEntitySnapshot &entity,
                   const std::shared_ptr<const ModelAsset> &asset,
                   const SceneViewportCamera &camera,
                   const EditorViewportState &viewport) {
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const glm::mat4 viewProjection = camera.projection * camera.view;
    if (!asset || !asset->localBounds.valid) {
        const auto point = projectPoint(glm::vec3(entity.world[3]),
                                        viewProjection, viewport);
        if (point) {
            drawList->AddCircleFilled(*point, 4.0f, kSelectionColor);
            drawList->AddCircle(*point, 8.0f, kSelectionColor, 16, 1.5f);
        }
        return;
    }

    std::array<std::optional<ImVec2>, 8> projected;
    for (int index = 0; index < 8; ++index) {
        const glm::vec3 local{
            (index & 1) ? asset->localBounds.max.x
                        : asset->localBounds.min.x,
            (index & 2) ? asset->localBounds.max.y
                        : asset->localBounds.min.y,
            (index & 4) ? asset->localBounds.max.z
                        : asset->localBounds.min.z};
        projected[index] = projectPoint(
            glm::vec3(entity.world * glm::vec4(local, 1.0f)),
            viewProjection, viewport);
    }
    constexpr std::array<std::array<int, 2>, 12> edges{{
        {{0, 1}}, {{0, 2}}, {{0, 4}}, {{1, 3}}, {{1, 5}}, {{2, 3}},
        {{2, 6}}, {{3, 7}}, {{4, 5}}, {{4, 6}}, {{5, 7}}, {{6, 7}},
    }};
    for (const auto &edge : edges) {
        if (projected[edge[0]] && projected[edge[1]]) {
            drawList->AddLine(*projected[edge[0]], *projected[edge[1]],
                              kSelectionColor, 1.5f);
        }
    }
}

} // namespace

void SceneViewportController::beginFrame() {
    ImGuizmo::BeginFrame();
    pointerOverGizmo_ = false;
    dragDropActive_ = ImGui::GetDragDropPayload() != nullptr;
}

void SceneViewportController::drawToolbar() {
    const auto button = [this](const char *label, const char *tooltip,
                               GizmoOperation operation) {
        const bool active = operation_ == operation;
        if (active) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label, ImVec2(24.0f, 0.0f)))
            operation_ = operation;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    button("Q", "Select", GizmoOperation::Select);
    button("W", "Translate", GizmoOperation::Translate);
    button("E", "Rotate", GizmoOperation::Rotate);
    button("R", "Scale", GizmoOperation::Scale);

    int space = space_ == GizmoSpace::Local ? 0 : 1;
    const char *labels[] = {"Local", "World"};
    ImGui::BeginDisabled(operation_ == GizmoOperation::Scale);
    if (editor::segmentedControl("GizmoSpace", space, labels, 2, 104.0f))
        space_ = space == 0 ? GizmoSpace::Local : GizmoSpace::World;
    ImGui::EndDisabled();
}

std::optional<SceneViewportController::Ray>
SceneViewportController::viewportRay(
    const EditorViewportState &viewport,
    const SceneViewportCamera &camera, const glm::vec2 &point) {
    if (!viewport.valid || viewport.logicalWidth <= 0.0f ||
        viewport.logicalHeight <= 0.0f)
        return std::nullopt;
    const float u = (point.x - viewport.minX) / viewport.logicalWidth;
    const float v = (point.y - viewport.minY) / viewport.logicalHeight;
    const glm::vec2 ndc{u * 2.0f - 1.0f, v * 2.0f - 1.0f};
    const glm::mat4 inverseViewProjection =
        glm::inverse(camera.projection * camera.view);
    glm::vec4 nearPoint =
        inverseViewProjection * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 farPoint =
        inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
    if (std::abs(nearPoint.w) <= 1.0e-8f ||
        std::abs(farPoint.w) <= 1.0e-8f)
        return std::nullopt;
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 direction =
        glm::normalize(glm::vec3(farPoint - nearPoint));
    if (!finiteVector(direction))
        return std::nullopt;
    return Ray{glm::vec3(nearPoint), direction};
}

void SceneViewportController::drawOverlay(
    const EditorViewportState &viewport, const SceneViewportCamera &camera,
    SceneEditorSession &session, const SceneViewportActions &actions) {
    if (!viewport.valid || !viewport.visible || !session.active()) {
        cancelManipulation();
        return;
    }
    activeSession_ = &session;
    std::shared_ptr<RuntimeWorld> world = session.world();
    if (!world) {
        cancelManipulation();
        return;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto ray = viewportRay(viewport, camera, {mouse.x, mouse.y});
    bool acceptedModelPayload = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                editor::kModelAssetPayload,
                ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            acceptedModelPayload = true;
            dragDropActive_ = true;
            std::string modelId;
            if (payload->Data && payload->DataSize > 0) {
                const char *value = static_cast<const char *>(payload->Data);
                size_t length = 0;
                const size_t capacity =
                    static_cast<size_t>(payload->DataSize);
                while (length < capacity && value[length] != '\0')
                    ++length;
                modelId.assign(value, length);
            }
            if (ray && !modelId.empty()) {
                const auto point = placementPoint(
                    *ray, camera, *world, session.selection());
                if (point) {
                    if (const auto screen = projectPoint(
                            *point, camera.projection * camera.view,
                            viewport)) {
                        ImDrawList *drawList = ImGui::GetWindowDrawList();
                        drawList->AddCircleFilled(*screen, 5.0f,
                                                  kPlacementColor);
                        drawList->AddCircle(*screen, 11.0f,
                                            kPlacementColor, 20, 2.0f);
                        const std::string name = actions.modelDisplayName
                                                     ? actions.modelDisplayName(
                                                           modelId)
                                                     : modelId;
                        drawList->AddText(
                            ImVec2(screen->x + 12.0f, screen->y + 8.0f),
                            kPlacementColor, name.c_str());
                    }
                    if (payload->IsDelivery() && actions.instantiateModel)
                        actions.instantiateModel(modelId, *point);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    const bool shortcutsAllowed =
        viewport.focused && !camera.cameraDragging &&
        !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt &&
        !ImGui::GetIO().KeySuper;
    if (shortcutsAllowed) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            operation_ = GizmoOperation::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            operation_ = GizmoOperation::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            operation_ = GizmoOperation::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            operation_ = GizmoOperation::Scale;
    }

    std::optional<RuntimeEntitySnapshot> selected;
    if (session.selection())
        selected = world->entity(world->find(*session.selection()));
    if (!selected && manipulationActive_)
        cancelManipulation();
    if (selected && manipulationActive_ && manipulatedEntity_ &&
        *manipulatedEntity_ != selected->id) {
        cancelManipulation();
    }
    if (manipulationActive_ &&
        (operation_ == GizmoOperation::Select || camera.cameraDragging)) {
        cancelManipulation();
    }
    selected.reset();
    if (session.selection())
        selected = world->entity(world->find(*session.selection()));
    activeSession_ = &session;

    if (selected) {
        drawSelection(*selected, world->modelAsset(selected->handle), camera,
                      viewport);
        const bool activeCameraSelected =
            session.cameraMode() == EditorCameraMode::ActiveScene &&
            world->activeCameraId() &&
            *world->activeCameraId() == selected->id;
        const bool atmosphereTransformLocked =
            selected->atmosphere &&
            (operation_ == GizmoOperation::Rotate ||
             operation_ == GizmoOperation::Scale);
        if (activeCameraSelected) {
            if (manipulationActive_)
                cancelManipulation();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(viewport.minX + 10.0f, viewport.minY + 10.0f),
                IM_COL32(255, 196, 84, 255),
                "Switch to Editor Camera to edit the active camera");
        } else if (atmosphereTransformLocked) {
            if (manipulationActive_)
                cancelManipulation();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(viewport.minX + 10.0f, viewport.minY + 10.0f),
                IM_COL32(255, 196, 84, 255),
                "Sky Atmosphere only supports translation");
        } else if (operation_ != GizmoOperation::Select &&
                   !camera.cameraDragging) {
            glm::mat4 gizmoProjection = camera.projection;
            gizmoProjection[1][1] *= -1.0f;
            glm::mat4 manipulatedWorld = selected->world;
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(viewport.minX, viewport.minY,
                              viewport.logicalWidth,
                              viewport.logicalHeight);
            const ImGuizmo::MODE mode =
                operation_ == GizmoOperation::Scale ||
                        space_ == GizmoSpace::Local
                    ? ImGuizmo::LOCAL
                    : ImGuizmo::WORLD;
            const bool changed = ImGuizmo::Manipulate(
                glm::value_ptr(camera.view), glm::value_ptr(gizmoProjection),
                imGuizmoOperation(operation_), mode,
                glm::value_ptr(manipulatedWorld));
            pointerOverGizmo_ = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
            const bool usingNow = ImGuizmo::IsUsing();
            if (usingNow && !manipulationActive_) {
                session.beginContinuousEdit("Transform Entity");
                manipulationActive_ = true;
                manipulationChanged_ = false;
                manipulatedEntity_ = selected->id;
            }
            if (usingNow && changed) {
                glm::mat4 localMatrix = manipulatedWorld;
                bool localMatrixValid = true;
                if (selected->parent) {
                    const auto parent =
                        world->entity(world->find(*selected->parent));
                    if (!parent) {
                        localMatrixValid = false;
                        if (actions.reportError)
                            actions.reportError("invalid_parent");
                    } else {
                        const float determinant =
                            glm::determinant(parent->world);
                        if (!std::isfinite(determinant) ||
                            std::abs(determinant) < 1.0e-8f) {
                            localMatrixValid = false;
                            if (actions.reportError)
                                actions.reportError(
                                    "transform_not_decomposable");
                        } else {
                            localMatrix = glm::inverse(parent->world) *
                                          manipulatedWorld;
                        }
                    }
                }
                SceneTransformDocument local;
                if (localMatrixValid &&
                    decomposeSceneTransform(localMatrix, local)) {
                    world->setTransform(selected->handle, local);
                    world->update(0.0f, 0.0f);
                    manipulationChanged_ = true;
                } else if (actions.reportError) {
                    actions.reportError("transform_not_decomposable");
                }
            }
            if (manipulationActive_ && !usingNow) {
                if (manipulationChanged_)
                    session.commitContinuousEdit();
                else
                    session.cancelContinuousEdit();
                manipulationActive_ = false;
                manipulationChanged_ = false;
                manipulatedEntity_.reset();
            }
        }
    }

    const bool pick = viewport.hovered && ray &&
                      ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                      !camera.cameraDragging && !pointerOverGizmo_ &&
                      !acceptedModelPayload && !dragDropActive_ &&
                      !ImGui::IsAnyItemActive();
    if (pick) {
        float closest = std::numeric_limits<float>::max();
        std::optional<PersistentEntityId> hit;
        for (const RuntimeEntitySnapshot &entity : world->entities()) {
            if (!entity.effectiveEnabled || !entity.modelInstance ||
                entity.modelBindingState != ModelBindingState::Ready)
                continue;
            const std::shared_ptr<const ModelAsset> asset =
                world->modelAsset(entity.handle);
            if (!asset || !asset->localBounds.valid)
                continue;
            const float determinant = glm::determinant(entity.world);
            if (!std::isfinite(determinant) ||
                std::abs(determinant) < 1.0e-8f)
                continue;
            const glm::mat4 inverseWorld = glm::inverse(entity.world);
            Ray localRay;
            localRay.origin = glm::vec3(
                inverseWorld * glm::vec4(ray->origin, 1.0f));
            localRay.direction = glm::vec3(
                inverseWorld * glm::vec4(ray->direction, 0.0f));
            float distance = 0.0f;
            if (intersectRayBounds(localRay.origin, localRay.direction,
                                   asset->localBounds, distance) &&
                distance < closest) {
                closest = distance;
                hit = entity.id;
            }
        }
        session.select(hit);
    }
}

bool SceneViewportController::blocksViewportInput() const {
    return manipulationActive_ || pointerOverGizmo_ || dragDropActive_;
}

void SceneViewportController::cancelManipulation() {
    if (manipulationActive_ && activeSession_)
        activeSession_->cancelContinuousEdit();
    manipulationActive_ = false;
    manipulationChanged_ = false;
    manipulatedEntity_.reset();
    activeSession_ = nullptr;
}

} // namespace vkr
