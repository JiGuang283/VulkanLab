#include "EditorWidgets.h"

#include <algorithm>
#include <string>

namespace vkr::editor {

ImVec4 statusColor(StatusTone tone) {
    switch (tone) {
    case StatusTone::Success:
        return ImVec4(0.31f, 0.78f, 0.47f, 1.0f);
    case StatusTone::Warning:
        return ImVec4(0.95f, 0.69f, 0.25f, 1.0f);
    case StatusTone::Error:
        return ImVec4(0.91f, 0.34f, 0.34f, 1.0f);
    case StatusTone::Info:
        return ImVec4(0.32f, 0.66f, 0.93f, 1.0f);
    case StatusTone::Neutral:
    default:
        return ImVec4(0.54f, 0.57f, 0.61f, 1.0f);
    }
}

void statusIndicator(const char *label, StatusTone tone,
                     const char *tooltip) {
    const float radius = std::max(3.0f, ImGui::GetFontSize() * 0.24f);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    const ImVec2 center(cursor.x + radius,
                        cursor.y + lineHeight * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        center, radius, ImGui::ColorConvertFloat4ToU32(statusColor(tone)));
    ImGui::Dummy(ImVec2(radius * 2.0f + 1.0f, lineHeight));
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label ? label : "");
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

bool beginPropertyGrid(const char *id, float labelFraction) {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 2, flags))
        return false;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch,
                            std::clamp(labelFraction, 0.2f, 0.8f));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch,
                            1.0f - std::clamp(labelFraction, 0.2f, 0.8f));
    return true;
}

void propertyLabel(const char *label, const char *tooltip) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

void endPropertyGrid() { ImGui::EndTable(); }

void pathValue(std::string_view value) {
    const std::string text(value);
    ImGui::TextUnformatted(text.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nRight-click to copy", text.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::SetClipboardText(text.c_str());
}

void emptyState(const char *message) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", message);
    ImGui::Spacing();
}

bool segmentedControl(const char *id, int &selected,
                      const char *const *labels, size_t labelCount) {
    if (!labels || labelCount == 0)
        return false;

    bool changed = false;
    ImGui::PushID(id);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = std::max(
        1.0f, (ImGui::GetContentRegionAvail().x -
               spacing * static_cast<float>(labelCount - 1)) /
                  static_cast<float>(labelCount));
    for (size_t i = 0; i < labelCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        const bool active = selected == static_cast<int>(i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(
                                      ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(labels[i], ImVec2(width, 0.0f)) && !active) {
            selected = static_cast<int>(i);
            changed = true;
        }
        if (active)
            ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

} // namespace vkr::editor
