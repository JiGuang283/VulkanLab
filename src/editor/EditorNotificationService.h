#pragma once

#include "EditorWidgets.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vkr {

struct EditorNotification {
    uint64_t id = 0;
    editor::StatusTone tone = editor::StatusTone::Info;
    std::string title;
    std::string message;
    std::string actionLabel;
    std::function<void()> action;
    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::milliseconds duration{4000};
    bool persistent = false;
};

class EditorNotificationService {
  public:
    uint64_t push(editor::StatusTone tone, std::string title,
                  std::string message = {}, std::string actionLabel = {},
                  std::function<void()> action = {});
    void dismiss(uint64_t id);
    void update();
    void draw(float bottomInset = 28.0f);

    size_t errorCount() const;
    const std::vector<EditorNotification> &notifications() const {
        return notifications_;
    }

  private:
    uint64_t nextId_ = 1;
    std::vector<EditorNotification> notifications_;
};

} // namespace vkr
