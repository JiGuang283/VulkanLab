#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct EditorUiState {
    static constexpr size_t kPerformanceHistorySize = 180;

    std::array<char, 128> materialSearch{};
    size_t selectedMaterialIndex = 0;
    uint64_t materialSceneGeneration =
        std::numeric_limits<uint64_t>::max();
    int selectedEnvironmentIndex = 0;
    std::optional<std::filesystem::path> environmentImportSource;
    std::array<char, 192> environmentDisplayName{};
    std::array<char, 128> environmentId{};
    std::vector<std::string> environmentProfileIds;
    int environmentProfileIndex = 0;
    bool requestEnvironmentImportModal = false;
    std::string environmentStatus;
    std::string environmentError;

    std::array<float, kPerformanceHistorySize> fpsHistory{};
    std::array<float, kPerformanceHistorySize> gpuHistory{};
    size_t performanceCursor = 0;
    size_t performanceCount = 0;
};

} // namespace vkr
