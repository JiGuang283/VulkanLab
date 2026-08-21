#pragma once

#include <array>
#include <string>

namespace vkr {

enum class EditorPendingActionKind {
    None,
    NewScene,
    LoadScene,
    CloseScene,
    Quit,
};

struct EditorUiState {
    std::array<char, 192> sceneDisplayName{};
    std::array<char, 128> sceneId{};
    bool requestNewSceneModal = false;
    bool requestOpenSceneModal = false;
    bool requestSaveAsModal = false;
    bool requestConvertPreviewModal = false;
    bool requestDirtyModal = false;
    bool requestSceneConflictModal = false;
    int openSceneIndex = -1;
    int pendingSceneIndex = -1;
    EditorPendingActionKind pendingAction = EditorPendingActionKind::None;
    bool quitConfirmed = false;
    std::string sceneStatus;
    std::string sceneError;
};

} // namespace vkr
