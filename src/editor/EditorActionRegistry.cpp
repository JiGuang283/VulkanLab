#include "EditorActionRegistry.h"

#include "EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace vkr {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return result;
}

bool actionEnabled(const EditorAction &action) {
    return !action.enabled || action.enabled();
}

} // namespace

void EditorActionRegistry::clear() { actions_.clear(); }

void EditorActionRegistry::add(EditorAction action) {
    if (action.id.empty() || !action.execute)
        return;
    auto existing = std::find_if(actions_.begin(), actions_.end(),
                                 [&](const EditorAction &candidate) {
                                     return candidate.id == action.id;
                                 });
    if (existing != actions_.end())
        *existing = std::move(action);
    else
        actions_.push_back(std::move(action));
}

const EditorAction *EditorActionRegistry::find(std::string_view id) const {
    const auto found = std::find_if(
        actions_.begin(), actions_.end(),
        [&](const EditorAction &action) { return action.id == id; });
    return found == actions_.end() ? nullptr : &*found;
}

bool EditorActionRegistry::invoke(std::string_view id) const {
    const EditorAction *action = find(id);
    if (!action || !actionEnabled(*action))
        return false;
    action->execute();
    return true;
}

void EditorCommandPalette::draw(const EditorActionRegistry &registry) {
    if (openRequested_) {
        ImGui::OpenPopup("Command Palette###VulkanLab.CommandPalette");
        openRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 380.0f),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Command Palette###VulkanLab.CommandPalette",
                                nullptr,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }
    if (ImGui::IsWindowAppearing()) {
        query_[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputTextWithHint(
        "##CommandQuery", "Search commands...", query_, sizeof(query_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string query = lower(query_);
    const EditorAction *first = nullptr;
    ImGui::BeginChild("CommandResults", ImVec2(0.0f, 0.0f), false);
    for (const EditorAction &action : registry.actions()) {
        const std::string haystack =
            lower(action.label + " " + action.keywords + " " + action.id);
        if (!query.empty() && haystack.find(query) == std::string::npos)
            continue;
        const bool enabled = actionEnabled(action);
        if (!first && enabled)
            first = &action;
        std::string label;
        if (editor::iconsAvailable() && action.icon) {
            label = action.icon;
            label += "  ";
        }
        label += action.label;
        if (!action.shortcut.empty())
            label += "\t" + action.shortcut;
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Selectable((label + "##" + action.id).c_str()) &&
            enabled) {
            action.execute();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    if (submitted && first) {
        first->execute();
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace vkr
