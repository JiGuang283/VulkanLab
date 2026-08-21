#include "EditorPreferences.h"

#include "core/Log.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace vkr {
namespace {

std::filesystem::path localAppDataPath() {
    wchar_t *value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") != 0 || !value ||
        length <= 1) {
        if (value)
            std::free(value);
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    }
    const std::filesystem::path result(value);
    std::free(value);
    return result;
}

const char *workspaceValue(EditorWorkspacePreset preset) {
    switch (preset) {
    case EditorWorkspacePreset::Scene:
        return "scene";
    case EditorWorkspacePreset::LookDev:
        return "lookdev";
    case EditorWorkspacePreset::Debug:
        return "debug";
    case EditorWorkspacePreset::Compact:
        return "compact";
    }
    return "scene";
}

EditorWorkspacePreset parseWorkspace(const std::string &value) {
    if (value == "lookdev")
        return EditorWorkspacePreset::LookDev;
    if (value == "debug" || value == "debugging")
        return EditorWorkspacePreset::Debug;
    if (value == "compact")
        return EditorWorkspacePreset::Compact;
    // The pre-v4 name maps to the new scene-authoring workspace.
    if (value == "viewport" || value == "scene")
        return EditorWorkspacePreset::Scene;
    return EditorWorkspacePreset::Scene;
}

const char *gizmoOperationValue(GizmoOperation value) {
    switch (value) {
    case GizmoOperation::Select:
        return "select";
    case GizmoOperation::Translate:
        return "translate";
    case GizmoOperation::Rotate:
        return "rotate";
    case GizmoOperation::Scale:
        return "scale";
    }
    return "translate";
}

GizmoOperation parseGizmoOperation(const std::string &value) {
    if (value == "select")
        return GizmoOperation::Select;
    if (value == "rotate")
        return GizmoOperation::Rotate;
    if (value == "scale")
        return GizmoOperation::Scale;
    return GizmoOperation::Translate;
}

void replaceAtomically(const std::filesystem::path &temporary,
                       const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not replace editor preferences: " +
                                 std::to_string(error));
    }
}

} // namespace

const char *editorWorkspaceName(EditorWorkspacePreset preset) {
    switch (preset) {
    case EditorWorkspacePreset::Scene:
        return "Scene";
    case EditorWorkspacePreset::LookDev:
        return "LookDev";
    case EditorWorkspacePreset::Debug:
        return "Debug";
    case EditorWorkspacePreset::Compact:
        return "Compact";
    }
    return "Scene";
}

EditorStoragePaths EditorStoragePaths::resolve(const std::string &projectId) {
    const std::string safeProject = projectId.empty() ? "default" : projectId;
    EditorStoragePaths paths;
    paths.root = localAppDataPath() / L"VulkanLab" / L"Editor" /
                 std::filesystem::u8path(safeProject);
    paths.preferences = paths.root / L"preferences.json";
    paths.layout = paths.root / L"layout.ini";
    return paths;
}

EditorPreferencesStore::EditorPreferencesStore(EditorStoragePaths paths)
    : paths_(std::move(paths)) {
    load();
}

EditorPreferencesStore::~EditorPreferencesStore() {
    try {
        flush();
    } catch (const std::exception &error) {
        VKR_LOG_ERROR("Editor", "Could not save preferences: {}",
                      error.what());
    }
}

EditorPreferences &EditorPreferencesStore::edit() {
    dirty_ = true;
    changedAt_ = std::chrono::steady_clock::now();
    return preferences_;
}

void EditorPreferencesStore::update() {
    if (!dirty_)
        return;
    if (std::chrono::steady_clock::now() - changedAt_ >=
        std::chrono::seconds(1)) {
        save();
    }
}

void EditorPreferencesStore::flush() {
    if (dirty_)
        save();
}

void EditorPreferencesStore::load() {
    std::ifstream input(paths_.preferences, std::ios::binary);
    if (!input)
        return;
    try {
        nlohmann::json root;
        input >> root;
        if (root.at("schemaVersion").get<int>() !=
            EditorPreferences::kSchemaVersion) {
            throw std::runtime_error("unsupported schema version");
        }
        preferences_.workspace =
            parseWorkspace(root.value("workspace", std::string{"scene"}));
        preferences_.contentView =
            root.value("contentView", std::string{"grid"}) == "list"
                ? ContentBrowserViewMode::List
                : ContentBrowserViewMode::Grid;
        preferences_.gizmoOperation = parseGizmoOperation(
            root.value("gizmoOperation", std::string{"translate"}));
        preferences_.gizmoSpace =
            root.value("gizmoSpace", std::string{"world"}) == "local"
                ? GizmoSpace::Local
                : GizmoSpace::World;
        preferences_.cameraMoveSpeed = std::clamp(
            root.value("cameraMoveSpeed", 2.0f), 0.1f, 100.0f);
        preferences_.bottomDrawerHeight = std::clamp(
            root.value("bottomDrawerHeight", 240.0f), 180.0f, 360.0f);
        preferences_.renderAdvanced =
            root.value("renderAdvanced", false);
        preferences_.bottomDrawerExpanded =
            root.value("bottomDrawerExpanded", false);
        const nlohmann::json overlays =
            root.value("viewportOverlays", nlohmann::json::object());
        preferences_.showBounds = overlays.value("bounds", true);
        preferences_.showLights = overlays.value("lights", true);
        preferences_.showProbes = overlays.value("probes", true);
    } catch (const std::exception &error) {
        preferences_ = {};
        VKR_LOG_WARN("Editor", "Ignoring invalid preferences '{}': {}",
                     paths_.preferences.u8string(), error.what());
    }
}

void EditorPreferencesStore::save() {
    std::error_code error;
    std::filesystem::create_directories(paths_.root, error);
    if (error)
        throw std::runtime_error("Could not create editor settings directory");

    nlohmann::json root{
        {"schemaVersion", EditorPreferences::kSchemaVersion},
        {"workspace", workspaceValue(preferences_.workspace)},
        {"contentView", preferences_.contentView ==
                            ContentBrowserViewMode::List
                        ? "list"
                        : "grid"},
        {"gizmoOperation", gizmoOperationValue(preferences_.gizmoOperation)},
        {"gizmoSpace", preferences_.gizmoSpace == GizmoSpace::Local
                           ? "local"
                           : "world"},
        {"cameraMoveSpeed", preferences_.cameraMoveSpeed},
        {"bottomDrawerHeight", preferences_.bottomDrawerHeight},
        {"renderAdvanced", preferences_.renderAdvanced},
        {"bottomDrawerExpanded", preferences_.bottomDrawerExpanded},
        {"viewportOverlays",
         {{"bounds", preferences_.showBounds},
          {"lights", preferences_.showLights},
          {"probes", preferences_.showProbes}}},
    };
    const std::filesystem::path temporary =
        paths_.preferences.parent_path() /
        (paths_.preferences.filename().wstring() + L".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not open editor preferences");
        output << root.dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not write editor preferences");
    }
    replaceAtomically(temporary, paths_.preferences);
    dirty_ = false;
}

} // namespace vkr
