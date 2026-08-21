#include "EditorNotificationService.h"

#include "EditorIcons.h"
#include "EditorTheme.h"

#include <imgui.h>

#include <algorithm>

namespace vkr {
namespace {

const char *toneIcon(editor::StatusTone tone) {
    switch (tone) {
    case editor::StatusTone::Success:
        return icons::Success;
    case editor::StatusTone::Warning:
        return icons::Warning;
    case editor::StatusTone::Error:
        return icons::Error;
    case editor::StatusTone::Info:
    case editor::StatusTone::Neutral:
        return icons::Activity;
    }
    return icons::Activity;
}

} // namespace

uint64_t EditorNotificationService::push(
    editor::StatusTone tone, std::string title, std::string message,
    std::string actionLabel, std::function<void()> action) {
    EditorNotification notification;
    notification.id = nextId_++;
    notification.tone = tone;
    notification.title = std::move(title);
    notification.message = std::move(message);
    notification.actionLabel = std::move(actionLabel);
    notification.action = std::move(action);
    notification.createdAt = std::chrono::steady_clock::now();
    notification.persistent = tone == editor::StatusTone::Error;
    if (tone == editor::StatusTone::Warning)
        notification.duration = std::chrono::milliseconds(8000);
    notifications_.push_back(std::move(notification));
    if (notifications_.size() > 8)
        notifications_.erase(notifications_.begin());
    return notifications_.back().id;
}

void EditorNotificationService::dismiss(uint64_t id) {
    notifications_.erase(
        std::remove_if(notifications_.begin(), notifications_.end(),
                       [&](const EditorNotification &value) {
                           return value.id == id;
                       }),
        notifications_.end());
}

void EditorNotificationService::update() {
    const auto now = std::chrono::steady_clock::now();
    notifications_.erase(
        std::remove_if(notifications_.begin(), notifications_.end(),
                       [&](const EditorNotification &value) {
                           return !value.persistent &&
                                  now - value.createdAt >= value.duration;
                       }),
        notifications_.end());
}

void EditorNotificationService::draw(float bottomInset) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float y = viewport->WorkPos.y + viewport->WorkSize.y - bottomInset;
    const size_t begin = notifications_.size() > 4
                             ? notifications_.size() - 4
                             : 0;
    for (size_t reverse = notifications_.size(); reverse > begin; --reverse) {
        EditorNotification &notification = notifications_[reverse - 1];
        const std::string windowName =
            "Notification###VulkanLab.Notification." +
            std::to_string(notification.id);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f, y),
            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f),
                                            ImVec2(420.0f, 180.0f));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin(windowName.c_str(), nullptr, flags)) {
            if (editor::iconsAvailable()) {
                ImGui::TextColored(editor::statusColor(notification.tone),
                                   "%s", toneIcon(notification.tone));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(notification.title.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 28.0f);
            if (ImGui::SmallButton((std::string("x##") +
                                    std::to_string(notification.id)).c_str())) {
                const uint64_t id = notification.id;
                ImGui::End();
                dismiss(id);
                continue;
            }
            if (!notification.message.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340.0f);
                ImGui::TextDisabled("%s", notification.message.c_str());
                ImGui::PopTextWrapPos();
            }
            if (notification.action &&
                ImGui::SmallButton(notification.actionLabel.empty()
                                       ? "Open"
                                       : notification.actionLabel.c_str())) {
                notification.action();
            }
            y -= ImGui::GetWindowHeight() + 8.0f;
        }
        ImGui::End();
    }
}

size_t EditorNotificationService::errorCount() const {
    return static_cast<size_t>(std::count_if(
        notifications_.begin(), notifications_.end(),
        [](const EditorNotification &value) {
            return value.tone == editor::StatusTone::Error;
        }));
}

} // namespace vkr
