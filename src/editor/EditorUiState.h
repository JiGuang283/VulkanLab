#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    static constexpr size_t kPerformanceHistorySize = 180;

    std::array<char, 128> materialSearch{};
    size_t selectedMaterialIndex = 0;
    uint64_t materialSceneGeneration =
        std::numeric_limits<uint64_t>::max();

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

    std::array<float, kPerformanceHistorySize> fpsHistory{};
    std::array<float, kPerformanceHistorySize> gpuHistory{};
    size_t performanceCursor = 0;
    size_t performanceCount = 0;
};

} // namespace vkr
