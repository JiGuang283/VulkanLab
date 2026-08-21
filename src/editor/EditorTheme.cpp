#include "EditorTheme.h"

#include <imgui.h>
#include <GLFW/glfw3.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <filesystem>

namespace vkr::editor {
namespace {

bool gIconsAvailable = false;

ImVec4 color(float r, float g, float b, float a = 1.0f) {
    return ImVec4(r, g, b, a);
}

} // namespace

void applyEditorTheme(GLFWwindow *window,
                      const std::filesystem::path &iconFontPath) {
    float xScale = 1.0f;
    float yScale = 1.0f;
    if (window)
        glfwGetWindowContentScale(window, &xScale, &yScale);
    const float dpiScale = std::clamp(std::max(xScale, yScale), 1.0f, 2.0f);

    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig{};
    fontConfig.SizePixels = 15.0f * dpiScale;

    bool fontLoaded = false;
    wchar_t windowsDirectory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) != 0) {
        const std::filesystem::path fontPath =
            std::filesystem::path(windowsDirectory) / L"Fonts" /
            L"segoeui.ttf";
        if (std::filesystem::exists(fontPath)) {
            fontLoaded = io.Fonts->AddFontFromFileTTF(
                             fontPath.string().c_str(),
                             fontConfig.SizePixels, &fontConfig) != nullptr;
        }
    }
    if (!fontLoaded)
        io.Fonts->AddFontDefault(&fontConfig);

    gIconsAvailable = false;
    if (!iconFontPath.empty() && std::filesystem::is_regular_file(iconFontPath)) {
        static constexpr ImWchar kIconRanges[] = {0xe000, 0xe7ff, 0};
        ImFontConfig iconConfig{};
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = fontConfig.SizePixels;
        iconConfig.GlyphMaxAdvanceX = fontConfig.SizePixels;
        gIconsAvailable = io.Fonts->AddFontFromFileTTF(
                              iconFontPath.string().c_str(),
                              fontConfig.SizePixels, &iconConfig,
                              kIconRanges) != nullptr;
    }

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 9.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = color(0.90f, 0.91f, 0.93f);
    colors[ImGuiCol_TextDisabled] = color(0.51f, 0.54f, 0.58f);
    colors[ImGuiCol_WindowBg] = color(0.105f, 0.115f, 0.125f);
    colors[ImGuiCol_ChildBg] = color(0.105f, 0.115f, 0.125f);
    colors[ImGuiCol_PopupBg] = color(0.13f, 0.14f, 0.15f, 0.98f);
    colors[ImGuiCol_Border] = color(0.24f, 0.26f, 0.28f);
    colors[ImGuiCol_BorderShadow] = color(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = color(0.16f, 0.175f, 0.19f);
    colors[ImGuiCol_FrameBgHovered] = color(0.21f, 0.25f, 0.29f);
    colors[ImGuiCol_FrameBgActive] = color(0.23f, 0.29f, 0.35f);
    colors[ImGuiCol_TitleBg] = color(0.09f, 0.10f, 0.11f);
    colors[ImGuiCol_TitleBgActive] = color(0.12f, 0.14f, 0.16f);
    colors[ImGuiCol_MenuBarBg] = color(0.10f, 0.11f, 0.12f);
    colors[ImGuiCol_ScrollbarBg] = color(0.08f, 0.09f, 0.10f);
    colors[ImGuiCol_ScrollbarGrab] = color(0.27f, 0.29f, 0.31f);
    colors[ImGuiCol_ScrollbarGrabHovered] = color(0.35f, 0.38f, 0.41f);
    colors[ImGuiCol_ScrollbarGrabActive] = color(0.43f, 0.47f, 0.51f);
    colors[ImGuiCol_CheckMark] = color(0.32f, 0.66f, 0.93f);
    colors[ImGuiCol_SliderGrab] = color(0.32f, 0.66f, 0.93f);
    colors[ImGuiCol_SliderGrabActive] = color(0.43f, 0.74f, 0.98f);
    colors[ImGuiCol_Button] = color(0.19f, 0.22f, 0.25f);
    colors[ImGuiCol_ButtonHovered] = color(0.25f, 0.34f, 0.42f);
    colors[ImGuiCol_ButtonActive] = color(0.27f, 0.43f, 0.56f);
    colors[ImGuiCol_Header] = color(0.19f, 0.24f, 0.28f);
    colors[ImGuiCol_HeaderHovered] = color(0.24f, 0.34f, 0.42f);
    colors[ImGuiCol_HeaderActive] = color(0.27f, 0.43f, 0.56f);
    colors[ImGuiCol_Separator] = color(0.24f, 0.26f, 0.28f);
    colors[ImGuiCol_SeparatorHovered] = color(0.35f, 0.58f, 0.76f);
    colors[ImGuiCol_SeparatorActive] = color(0.39f, 0.68f, 0.91f);
    colors[ImGuiCol_ResizeGrip] = color(0.30f, 0.55f, 0.72f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = color(0.35f, 0.64f, 0.86f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = color(0.39f, 0.68f, 0.91f, 0.95f);
    colors[ImGuiCol_Tab] = color(0.13f, 0.15f, 0.17f);
    colors[ImGuiCol_TabHovered] = color(0.25f, 0.38f, 0.48f);
    colors[ImGuiCol_TabSelected] = color(0.20f, 0.31f, 0.39f);
    colors[ImGuiCol_TabDimmed] = color(0.10f, 0.11f, 0.12f);
    colors[ImGuiCol_TabDimmedSelected] = color(0.16f, 0.20f, 0.23f);
    colors[ImGuiCol_DockingPreview] = color(0.32f, 0.66f, 0.93f, 0.55f);
    colors[ImGuiCol_DockingEmptyBg] = color(0.07f, 0.075f, 0.08f);
    colors[ImGuiCol_TableHeaderBg] = color(0.15f, 0.17f, 0.19f);
    colors[ImGuiCol_TableBorderStrong] = color(0.26f, 0.28f, 0.30f);
    colors[ImGuiCol_TableBorderLight] = color(0.20f, 0.22f, 0.24f);
    colors[ImGuiCol_TableRowBg] = color(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = color(1.0f, 1.0f, 1.0f, 0.025f);

    style.ScaleAllSizes(dpiScale);
}

bool iconsAvailable() { return gIconsAvailable; }

} // namespace vkr::editor
