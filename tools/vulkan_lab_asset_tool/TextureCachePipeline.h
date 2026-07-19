#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vkr::assettool {

struct TextureBuildWorkItem {
    size_t index = 0;
    int32_t imageIndex = -1;
    std::string semantic;
    uint64_t estimatedMemoryBytes = 0;
};

struct TextureBuildWorkResult {
    size_t index = 0;
    bool success = false;
    bool cancelled = false;
    bool reused = false;
    uint32_t exitCode = 0;
    double durationMs = 0.0;
    std::string error;
};

struct TextureBuildSchedulerOptions {
    uint32_t maxWorkers = 1;
    uint64_t memoryBudgetBytes = 0;
};

struct TextureBuildScheduleStats {
    uint32_t peakWorkers = 0;
    uint64_t peakReservedBytes = 0;
};

using TextureBuildWorker = std::function<TextureBuildWorkResult(
    const TextureBuildWorkItem &, const std::atomic_bool &)>;
using TextureBuildProgress =
    std::function<void(const char *event, const TextureBuildWorkItem &,
                       const TextureBuildWorkResult *)>;

std::vector<TextureBuildWorkResult>
executeTextureBuildPlan(const std::vector<TextureBuildWorkItem> &items,
                        const TextureBuildSchedulerOptions &options,
                        std::atomic_bool &cancelRequested,
                        const TextureBuildWorker &worker,
                        const TextureBuildProgress &progress = {},
                        TextureBuildScheduleStats *stats = nullptr);

} // namespace vkr::assettool
