#include "TextureCachePipeline.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace vkr::assettool {

std::vector<TextureBuildWorkResult> executeTextureBuildPlan(
    const std::vector<TextureBuildWorkItem> &items,
    const TextureBuildSchedulerOptions &options,
    std::atomic_bool &cancelRequested, const TextureBuildWorker &worker,
    const TextureBuildProgress &progress, TextureBuildScheduleStats *stats) {
    if (!worker)
        throw std::invalid_argument("texture build worker is required");
    if (items.empty())
        return {};

    const uint32_t workerCount =
        std::max(1u, std::min<uint32_t>(options.maxWorkers,
                                        static_cast<uint32_t>(items.size())));
    const uint64_t memoryBudget =
        options.memoryBudgetBytes == 0 ? UINT64_MAX : options.memoryBudgetBytes;

    std::vector<TextureBuildWorkResult> results(items.size());
    std::vector<bool> claimed(items.size(), false);
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t reservedBytes = 0;
    uint32_t activeWorkers = 0;
    TextureBuildScheduleStats observed;
    size_t completed = 0;

    const auto findReady = [&]() -> size_t {
        for (size_t i = 0; i < items.size(); ++i) {
            if (claimed[i])
                continue;
            const uint64_t estimate = items[i].estimatedMemoryBytes;
            if (reservedBytes == 0 ||
                estimate <=
                    memoryBudget - std::min(memoryBudget, reservedBytes))
                return i;
        }
        return items.size();
    };

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    for (uint32_t threadIndex = 0; threadIndex < workerCount; ++threadIndex) {
        threads.emplace_back([&] {
            while (true) {
                size_t itemIndex = items.size();
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] {
                        return cancelRequested.load() ||
                               completed == items.size() ||
                               findReady() != items.size();
                    });
                    if (cancelRequested.load() || completed == items.size())
                        return;
                    itemIndex = findReady();
                    if (itemIndex == items.size())
                        continue;
                    claimed[itemIndex] = true;
                    ++activeWorkers;
                    reservedBytes += items[itemIndex].estimatedMemoryBytes;
                    observed.peakWorkers =
                        std::max(observed.peakWorkers, activeWorkers);
                    observed.peakReservedBytes =
                        std::max(observed.peakReservedBytes, reservedBytes);
                }

                if (progress)
                    progress("artifact_started", items[itemIndex], nullptr);

                TextureBuildWorkResult result;
                result.index = items[itemIndex].index;
                try {
                    result = worker(items[itemIndex], cancelRequested);
                    result.index = items[itemIndex].index;
                } catch (const std::exception &exception) {
                    result.success = false;
                    result.cancelled = cancelRequested.load();
                    result.error = exception.what();
                } catch (...) {
                    result.success = false;
                    result.cancelled = cancelRequested.load();
                    result.error = "unknown texture build failure";
                }

                if (!result.success)
                    cancelRequested.store(true);
                results[itemIndex] = result;
                if (progress) {
                    progress(result.success ? "artifact_completed"
                                            : "artifact_failed",
                             items[itemIndex], &results[itemIndex]);
                }

                {
                    std::lock_guard lock(mutex);
                    reservedBytes -= items[itemIndex].estimatedMemoryBytes;
                    --activeWorkers;
                    ++completed;
                }
                changed.notify_all();
            }
        });
    }

    for (std::thread &thread : threads)
        thread.join();

    for (size_t i = 0; i < items.size(); ++i) {
        if (!claimed[i]) {
            results[i].index = items[i].index;
            results[i].cancelled = true;
            results[i].error = "cancelled before dispatch";
        }
    }
    if (stats)
        *stats = observed;
    return results;
}

} // namespace vkr::assettool
