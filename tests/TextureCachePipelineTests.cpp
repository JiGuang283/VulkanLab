#include "ProcessRunner.h"
#include "TextureCachePipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void updateMaximum(std::atomic_uint32_t &maximum, uint32_t value) {
    uint32_t current = maximum.load();
    while (current < value && !maximum.compare_exchange_weak(current, value)) {
    }
}

void testWorkerAndMemoryBounds() {
    std::vector<vkr::assettool::TextureBuildWorkItem> items;
    for (size_t i = 0; i < 8; ++i)
        items.push_back({i, static_cast<int32_t>(i), "linear", 10});

    std::atomic_bool cancelled{false};
    std::atomic_uint32_t active{0};
    std::atomic_uint32_t maximum{0};
    const auto results = vkr::assettool::executeTextureBuildPlan(
        items, {3, 30}, cancelled, [&](const auto &item, const auto &) {
            const uint32_t now = active.fetch_add(1) + 1;
            updateMaximum(maximum, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            active.fetch_sub(1);
            vkr::assettool::TextureBuildWorkResult result;
            result.index = item.index;
            result.success = true;
            return result;
        });
    require(results.size() == items.size(), "scheduler lost results");
    require(maximum.load() <= 3, "scheduler exceeded worker limit");
    require(std::all_of(results.begin(), results.end(),
                        [](const auto &result) { return result.success; }),
            "bounded scheduler unexpectedly failed");

    active.store(0);
    maximum.store(0);
    cancelled.store(false);
    for (auto &item : items)
        item.estimatedMemoryBytes = 80;
    vkr::assettool::executeTextureBuildPlan(
        items, {4, 100}, cancelled, [&](const auto &item, const auto &) {
            const uint32_t now = active.fetch_add(1) + 1;
            updateMaximum(maximum, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            active.fetch_sub(1);
            vkr::assettool::TextureBuildWorkResult result;
            result.index = item.index;
            result.success = true;
            return result;
        });
    require(maximum.load() == 1,
            "scheduler exceeded the configured memory budget");
}

void testOversizedTaskAndDeterministicResults() {
    std::vector<vkr::assettool::TextureBuildWorkItem> items{
        {0, 0, "normal", 200}, {1, 1, "linear", 40}, {2, 2, "srgb", 40}};
    std::atomic_bool cancelled{false};
    const auto results = vkr::assettool::executeTextureBuildPlan(
        items, {3, 100}, cancelled, [](const auto &item, const auto &) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(3 * (3 - item.index)));
            vkr::assettool::TextureBuildWorkResult result;
            result.index = item.index;
            result.success = true;
            result.exitCode = static_cast<uint32_t>(item.index + 10);
            return result;
        });
    require(results.size() == 3 && results[0].exitCode == 10 &&
                results[1].exitCode == 11 && results[2].exitCode == 12,
            "scheduler result order depends on completion order");
}

void testFailureStopsDispatch() {
    std::vector<vkr::assettool::TextureBuildWorkItem> items;
    for (size_t i = 0; i < 12; ++i)
        items.push_back({i, static_cast<int32_t>(i), "linear", 10});
    std::atomic_bool cancelled{false};
    std::atomic_uint32_t started{0};
    const auto results = vkr::assettool::executeTextureBuildPlan(
        items, {2, 20}, cancelled, [&](const auto &item, const auto &cancel) {
            started.fetch_add(1);
            vkr::assettool::TextureBuildWorkResult result;
            result.index = item.index;
            if (item.index == 1) {
                result.error = "synthetic encoder failure";
                result.exitCode = 17;
                return result;
            }
            for (int i = 0; i < 20 && !cancel.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            result.cancelled = cancel.load();
            result.success = !result.cancelled;
            return result;
        });
    require(cancelled.load(), "worker failure did not request cancellation");
    require(started.load() < items.size(),
            "fail-fast scheduler dispatched every pending task");
    require(results[1].exitCode == 17 &&
                results[1].error == "synthetic encoder failure",
            "worker failure details were not preserved");
}

class FakeProcessRunner final : public vkr::assettool::IProcessRunner {
  public:
    vkr::assettool::ProcessResult
    run(const vkr::assettool::ProcessRequest &,
        const std::atomic_bool &cancelRequested) override {
        ++calls;
        vkr::assettool::ProcessResult result;
        result.exitCode = cancelRequested.load() ? 1223u : exitCode;
        result.cancelled = cancelRequested.load();
        result.output = "fake output";
        return result;
    }
    void cancelAll() noexcept override { cancelled = true; }

    uint32_t exitCode = 0;
    uint32_t calls = 0;
    bool cancelled = false;
};

void testReplaceableProcessRunner() {
    FakeProcessRunner runner;
    std::atomic_bool cancel{false};
    const auto success = runner.run({}, cancel);
    require(success.exitCode == 0 && success.output == "fake output",
            "fake process runner result was not preserved");
    cancel.store(true);
    const auto cancelled = runner.run({}, cancel);
    require(cancelled.cancelled, "fake process runner did not observe cancel");
    runner.cancelAll();
    require(runner.cancelled && runner.calls == 2,
            "replaceable process runner contract is incomplete");
}

void testJobProcessCancellation() {
    const std::filesystem::path powershell =
        std::filesystem::path(std::getenv("SystemRoot")) /
        "System32/WindowsPowerShell/v1.0/powershell.exe";
    require(std::filesystem::is_regular_file(powershell),
            "PowerShell is unavailable for process cancellation test");

    vkr::assettool::Win32JobProcessRunner runner;
    std::atomic_bool cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.store(true);
    });
    const auto begin = std::chrono::steady_clock::now();
    const auto result =
        runner.run({powershell,
                    {L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-Command",
                     L"Start-Sleep -Seconds 30"}},
                   cancel);
    canceller.join();
    const double elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
            .count();
    require(result.cancelled, "Job Object runner ignored cancellation");
    require(elapsedSeconds < 5.0,
            "Job Object runner did not terminate the child promptly");
}

} // namespace

void runTextureCachePipelineTests() {
    testWorkerAndMemoryBounds();
    testOversizedTaskAndDeterministicResults();
    testFailureStopsDispatch();
    testReplaceableProcessRunner();
    testJobProcessCancellation();
}
