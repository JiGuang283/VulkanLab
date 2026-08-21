#pragma once

#include <filesystem>

struct GLFWwindow;

namespace vkr::editor {

void applyEditorTheme(GLFWwindow *window,
                      const std::filesystem::path &iconFontPath = {});
bool iconsAvailable();

} // namespace vkr::editor
