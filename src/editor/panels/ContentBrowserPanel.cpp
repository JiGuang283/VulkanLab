#include "ContentBrowserPanel.h"

#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/EditorWidgets.h"
#include "editor/EditorDragDrop.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace vkr {
namespace {

bool containsIgnoreCase(const std::string &value, const char *query) {
    if (!query || *query == '\0')
        return true;
    std::string haystack = value;
    std::string needle = query;
    const auto lower = [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    };
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    return haystack.find(needle) != std::string::npos;
}

bool statusMatches(const std::string &state, int filter) {
    if (filter == 0)
        return true;
    const bool ready = state == "Ready" || state == "Valid";
    return filter == 1 ? ready : !ready;
}

std::string compactLabel(const std::string &value, size_t limit = 22) {
    return value.size() <= limit ? value
                                 : value.substr(0, limit - 3) + "...";
}

} // namespace

void ContentBrowserPanel::draw(
    const ContentBrowserSnapshot &snapshot,
    const ContentBrowserActions &actions) const {
    ContentBrowserViewMode viewMode = snapshot.viewMode;
    if (editor::toggleIconButton("GridView", icons::Grid, "G", "Grid view",
                                 viewMode == ContentBrowserViewMode::Grid,
                                 ImVec2(28.0f, 0.0f))) {
        viewMode = ContentBrowserViewMode::Grid;
        if (actions.setViewMode)
            actions.setViewMode(viewMode);
    }
    ImGui::SameLine();
    if (editor::toggleIconButton("ListView", icons::List, "L", "List view",
                                 viewMode == ContentBrowserViewMode::List,
                                 ImVec2(28.0f, 0.0f))) {
        viewMode = ContentBrowserViewMode::List;
        if (actions.setViewMode)
            actions.setViewMode(viewMode);
    }
    ImGui::Separator();

    const auto drawCategory = [&](int category) {
        ImGui::PushID(category);
        switch (category) {
        case 0:
            drawAll(snapshot, actions);
            break;
        case 1:
            if (actions.drawScenes)
                actions.drawScenes();
            break;
        case 2:
            if (actions.drawModels)
                actions.drawModels();
            break;
        case 3:
            if (actions.drawEnvironments)
                actions.drawEnvironments();
            break;
        default:
            break;
        }
        ImGui::PopID();
    };

    if (ImGui::GetContentRegionAvail().x < 520.0f) {
        constexpr const char *categories[] = {
            "All", "Scenes", "Models / Primitives", "Environments"};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##ContentCategory", &category_, categories,
                     static_cast<int>(std::size(categories)));
        ImGui::Spacing();
        drawCategory(category_);
        return;
    }

    if (!ImGui::BeginTabBar("ContentBrowserCategories"))
        return;
    constexpr const char *categories[] = {
        "All", "Scenes", "Models / Primitives", "Environments"};
    for (int category = 0; category < static_cast<int>(std::size(categories));
         ++category) {
        if (!ImGui::BeginTabItem(categories[category]))
            continue;
        category_ = category;
        drawCategory(category);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void ContentBrowserPanel::drawAll(
    const ContentBrowserSnapshot &snapshot,
    const ContentBrowserActions &actions) const {
    ImGui::SetNextItemWidth(-112.0f);
    ImGui::InputTextWithHint("##Search", "Search...",
                             search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(104.0f);
    constexpr const char *filters[] = {"All", "Ready", "Issues"};
    ImGui::Combo("##Status", &statusFilter_, filters,
                 static_cast<int>(std::size(filters)));
    ImGui::Separator();

    if (!snapshot.scenes || !snapshot.assets) {
        editor::emptyState("Asset snapshots are unavailable.");
        return;
    }

    struct Item {
        const char *icon = nullptr;
        std::string name;
        std::string id;
        std::string status;
        enum class Kind { Scene, Model, Primitive, Environment } kind;
        int index = -1;
        bool enabled = true;
    };
    std::vector<Item> items;
    for (const SceneWorkflowItemSnapshot &scene :
         snapshot.scenes->nativeScenes) {
        items.push_back({icons::Scene, scene.displayName, scene.id,
                         scene.available ? "Ready" : "Unavailable",
                         Item::Kind::Scene, scene.index, scene.available});
    }
    for (const SceneWorkflowItemSnapshot &model : snapshot.scenes->models) {
        items.push_back({icons::Model, model.displayName, model.id,
                         model.artifactState, Item::Kind::Model, model.index,
                         model.available});
    }
    for (const EnginePrimitiveItemSnapshot &primitive :
         snapshot.scenes->enginePrimitives) {
        items.push_back({icons::Box, primitive.displayName, primitive.id,
                         "Ready", Item::Kind::Primitive, -1,
                         primitive.canInstantiate});
    }
    for (const EnvironmentAssetSnapshot &environment :
         snapshot.assets->environments) {
        items.push_back({icons::Environment, environment.displayName,
                         environment.id, environment.artifactState,
                         Item::Kind::Environment, -1, true});
    }
    items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &item) {
                    return (!containsIgnoreCase(item.name, search_.data()) &&
                            !containsIgnoreCase(item.id, search_.data())) ||
                           !statusMatches(item.status, statusFilter_);
                }),
                items.end());

    if (items.empty()) {
        editor::emptyState("No assets match the current filters.");
        return;
    }

    ImGui::BeginChild("AllAssetItems", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    const bool grid = snapshot.viewMode == ContentBrowserViewMode::Grid;
    const float tileWidth = 150.0f;
    const int columns = std::max(
        1, static_cast<int>(ImGui::GetContentRegionAvail().x /
                            (tileWidth + ImGui::GetStyle().ItemSpacing.x)));
    int column = 0;
    for (const Item &item : items) {
        ImGui::PushID((item.id + std::to_string(
                                      static_cast<int>(item.kind))).c_str());
        std::string label;
        if (editor::iconsAvailable()) {
            label = item.icon;
            label += grid ? "\n" : "  ";
        }
        label += grid ? compactLabel(item.name) : item.name;
        ImGui::BeginDisabled(!item.enabled);
        const bool activated = grid
                                   ? ImGui::Button(label.c_str(),
                                                   ImVec2(tileWidth, 62.0f))
                                   : ImGui::Selectable(label.c_str());
        ImGui::EndDisabled();
        if (activated && ImGui::IsMouseDoubleClicked(
                             ImGuiMouseButton_Left)) {
            if (item.kind == Item::Kind::Scene && actions.openScene)
                actions.openScene(item.index);
            else if (item.kind == Item::Kind::Model && actions.previewModel)
                actions.previewModel(item.index);
            else if (item.kind == Item::Kind::Environment &&
                     actions.assignEnvironment)
                actions.assignEnvironment(item.id);
        }
        if ((item.kind == Item::Kind::Model ||
             item.kind == Item::Kind::Primitive) &&
            item.enabled && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(editor::kModelAssetPayload,
                                      item.id.c_str(), item.id.size() + 1);
            ImGui::TextUnformatted(item.name.c_str());
            ImGui::TextDisabled("Drop into the Viewport");
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s\n%s", item.id.c_str(),
                              item.status.c_str());
        if (grid && (++column % columns) != 0)
            ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace vkr
