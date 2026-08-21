#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vkr {

struct EditorAction {
    std::string id;
    std::string label;
    std::string keywords;
    std::string shortcut;
    const char *icon = nullptr;
    std::function<bool()> enabled;
    std::function<void()> execute;
};

class EditorActionRegistry {
  public:
    void clear();
    void add(EditorAction action);
    const EditorAction *find(std::string_view id) const;
    bool invoke(std::string_view id) const;
    const std::vector<EditorAction> &actions() const { return actions_; }

  private:
    std::vector<EditorAction> actions_;
};

class EditorCommandPalette {
  public:
    void requestOpen() { openRequested_ = true; }
    void draw(const EditorActionRegistry &registry);

  private:
    char query_[128]{};
    bool openRequested_ = false;
};

} // namespace vkr
