#include "OutlinerPanel.h"

#include "editor/EditorDragDrop.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vkr {
namespace {

bool containsIgnoreCase(const std::string &value, const std::string &query) {
    if (query.empty())
        return true;
    auto lower = [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    };
    std::string valueLower = value;
    std::string queryLower = query;
    std::transform(valueLower.begin(), valueLower.end(), valueLower.begin(),
                   lower);
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                   lower);
    return valueLower.find(queryLower) != std::string::npos;
}

const OutlinerEntitySnapshot *findEntity(
    const OutlinerPanelSnapshot &snapshot, const PersistentEntityId &id) {
    const auto found = std::find_if(
        snapshot.entities.begin(), snapshot.entities.end(),
        [&](const OutlinerEntitySnapshot &entity) { return entity.id == id; });
    return found == snapshot.entities.end() ? nullptr : &*found;
}

std::optional<PersistentEntityId>
entityPayload(const ImGuiPayload *payload) {
    if (!payload || !payload->Data || payload->DataSize <= 0)
        return std::nullopt;
    const char *data = static_cast<const char *>(payload->Data);
    size_t length = 0;
    const size_t capacity = static_cast<size_t>(payload->DataSize);
    while (length < capacity && data[length] != '\0')
        ++length;
    return PersistentEntityId::parse(std::string_view(data, length));
}

} // namespace

void OutlinerPanel::beginRename(const PersistentEntityId &id,
                                const std::string &currentName) {
    std::snprintf(renameBuffer_.data(), renameBuffer_.size(), "%s",
                  currentName.c_str());
    renameTarget_ = id;
}

void OutlinerPanel::draw(const OutlinerPanelSnapshot &snapshot,
                         const OutlinerPanelActions &actions) {
    ImGui::SetNextItemWidth(-42.0f);
    ImGui::InputTextWithHint("##OutlinerSearch", "Search entities...",
                             search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot.editable);
    if (ImGui::Button("+"))
        ImGui::OpenPopup("CreateEntity");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create entity");
    if (ImGui::BeginPopup("CreateEntity")) {
        drawCreateMenu(actions, std::nullopt,
                       snapshot.canCreateAtmosphere);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("OutlinerTree", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    const bool rootSelected = false;
    if (ImGui::Selectable("Scene Root", rootSelected) && actions.select)
        actions.select(std::nullopt);
    if (snapshot.editable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                editor::kEntityPayload)) {
            if (payload->IsDelivery()) {
                if (const auto entity = entityPayload(payload);
                    entity && actions.reparent) {
                    actions.reparent(*entity, std::nullopt);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::Separator();
    const std::string query = search_.data();
    if (!query.empty()) {
        for (const OutlinerEntitySnapshot &entity : snapshot.entities) {
            if (containsIgnoreCase(entity.name, query))
                drawEntity(entity, snapshot, actions);
        }
    } else {
        for (const OutlinerEntitySnapshot &entity : snapshot.entities) {
            if (!entity.parent)
                drawEntity(entity, snapshot, actions);
        }
    }
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered() && actions.select) {
        actions.select(std::nullopt);
    }
    ImGui::EndChild();

    if (renameTarget_)
        ImGui::OpenPopup("Rename Entity");
    if (ImGui::BeginPopupModal("Rename Entity", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(320.0f);
        const bool submitted = ImGui::InputText(
            "Name", renameBuffer_.data(), renameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_AutoSelectAll);
        if ((submitted || ImGui::Button("Rename")) &&
            renameBuffer_[0] != '\0') {
            if (actions.rename)
                actions.rename(*renameTarget_, renameBuffer_.data());
            renameTarget_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            renameTarget_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void OutlinerPanel::drawEntity(const OutlinerEntitySnapshot &entity,
                               const OutlinerPanelSnapshot &snapshot,
                               const OutlinerPanelActions &actions) {
    const std::string id = entity.id.toString();
    bool hasChildren = false;
    for (const OutlinerEntitySnapshot &candidate : snapshot.entities) {
        if (candidate.parent && *candidate.parent == entity.id) {
            hasChildren = true;
            break;
        }
    }
    ImGui::PushID(id.c_str());
    bool enabled = entity.enabled;
    ImGui::BeginDisabled(!snapshot.editable);
    if (ImGui::Checkbox("##Enabled", &enabled) && actions.setEnabled)
        actions.setEnabled(entity.id, enabled);
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (entity.selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx("Entity", flags, "%s",
                                        entity.name.c_str());
    if (entity.hasModel && entity.modelState != ModelBindingState::Ready) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", modelBindingStateName(entity.modelState));
    }
    if (entity.lightLimitExceeded) {
        ImGui::SameLine();
        ImGui::TextDisabled("[Not uploaded]");
    }
    if (entity.hasReflectionProbe &&
        entity.reflectionProbeState != ModelBindingState::Ready) {
        ImGui::SameLine();
        ImGui::TextDisabled("[Probe %s]",
                            modelBindingStateName(
                                entity.reflectionProbeState));
    }
    if (ImGui::IsItemClicked() && actions.select)
        actions.select(entity.id);
    if (snapshot.editable && !entity.hasAtmosphere &&
        ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(editor::kEntityPayload, id.c_str(),
                                  id.size() + 1);
        ImGui::TextUnformatted(entity.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (snapshot.editable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                editor::kEntityPayload)) {
            if (payload->IsDelivery()) {
                if (const auto child = entityPayload(payload);
                    child && *child != entity.id && actions.reparent) {
                    actions.reparent(*child, entity.id);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        beginRename(entity.id, entity.name);
    }
    if (ImGui::BeginPopupContextItem("EntityActions")) {
        if (ImGui::BeginMenu("Create Child")) {
            drawCreateMenu(actions, entity.id,
                           snapshot.canCreateAtmosphere);
            ImGui::EndMenu();
        }
        ImGui::BeginDisabled(!snapshot.editable);
        if (ImGui::MenuItem("Rename", "F2")) {
            beginRename(entity.id, entity.name);
        }
        ImGui::BeginDisabled(entity.hasAtmosphere);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D") && actions.duplicate)
            actions.duplicate(entity.id);
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Delete", "Delete") && actions.remove)
            actions.remove(entity.id);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (hasChildren && open) {
        for (const OutlinerEntitySnapshot &child : snapshot.entities) {
            if (child.parent && *child.parent == entity.id)
                drawEntity(child, snapshot, actions);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void OutlinerPanel::drawCreateMenu(
    const OutlinerPanelActions &actions,
    std::optional<PersistentEntityId> parent,
    bool canCreateAtmosphere) {
    const auto create = [&](const char *label, OutlinerCreateKind kind) {
        if (ImGui::MenuItem(label) && actions.create)
            actions.create(kind, parent);
    };
    create("Empty", OutlinerCreateKind::Empty);
    create("Model", OutlinerCreateKind::Model);
    if (ImGui::BeginMenu("3D Object")) {
        create("Plane", OutlinerCreateKind::Plane);
        create("Cube", OutlinerCreateKind::Cube);
        create("Sphere", OutlinerCreateKind::Sphere);
        create("Cylinder", OutlinerCreateKind::Cylinder);
        create("Cone", OutlinerCreateKind::Cone);
        create("Capsule", OutlinerCreateKind::Capsule);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    create("Directional Light", OutlinerCreateKind::DirectionalLight);
    create("Point Light", OutlinerCreateKind::PointLight);
    create("Spot Light", OutlinerCreateKind::SpotLight);
    create("Camera", OutlinerCreateKind::Camera);
    create("Reflection Probe", OutlinerCreateKind::ReflectionProbe);
    ImGui::BeginDisabled(!canCreateAtmosphere || parent.has_value());
    create("Sky Atmosphere", OutlinerCreateKind::SkyAtmosphere);
    ImGui::EndDisabled();
}

} // namespace vkr
