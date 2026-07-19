#include "assets/AssetImportManager.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {

void requireImport(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool waitTerminal(const std::shared_ptr<vkr::AssetImportTask> &task,
                  std::chrono::milliseconds timeout =
                      std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (vkr::isTerminalAssetImportState(task->state.load()))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void testManagerProgressMergeAndFailure() {
    const auto root = std::filesystem::temp_directory_path() /
                      "vulkan_lab_import_manager_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    std::atomic_uint32_t calls{0};
    vkr::AssetImportExecutor executor =
        [&](const auto &, const vkr::AssetImportRequest &request,
            const std::atomic_bool &cancel, const auto &event,
            const auto &) -> vkr::AssetImportExecutionResult {
        ++calls;
        if (request.sceneId == "scene")
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        event({{"event", "started"},
               {"protocolVersion", 1},
               {"total", 3},
               {"reused", 1},
               {"workers", 2}});
        if (request.sceneId == "cancel") {
            while (!cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return {1223, true, {}};
        }
        if (request.sceneId == "failure")
            return {17, false, "synthetic importer failure"};
        if (request.sceneId == "crash")
            throw std::runtime_error("synthetic importer crash");
        if (request.sceneId == "bad-protocol") {
            event({{"event", "started"}, {"protocolVersion", 99}});
            return {};
        }
        event({{"event", "progress"},
               {"completed", 3},
               {"encoded", 2},
               {"reused", 1},
               {"failed", 0}});
        event({{"event", "publishing"}});
        event({{"event", "completed"},
               {"manifest", "manifest.json"},
               {"encoded", 2},
               {"reused", 1},
               {"failed", 0}});
        return {};
    };

    vkr::AssetImportManager manager(
        {root, root / "cache", {}, 2, 256}, executor);
    const auto first = manager.request({"scene", "profile",
                                        vkr::ImportReason::SceneLoad});
    const auto merged = manager.request({"scene", "profile",
                                         vkr::ImportReason::ManualReimport});
    requireImport(first->id == merged->id,
                  "same scene/profile import was not merged");
    requireImport((first->id & vkr::AssetImportManager::kTaskIdMask) != 0,
                  "import task ID is not isolated from load task IDs");
    requireImport(waitTerminal(first), "successful import did not finish");
    if (first->state != vkr::AssetImportState::Completed ||
        first->encodedArtifacts != 2 || first->reusedArtifacts != 1 ||
        first->completedArtifacts != 3) {
        throw std::runtime_error(
            std::string("import progress was not retained: state=") +
            vkr::assetImportStateName(first->state.load()) +
            " encoded=" + std::to_string(first->encodedArtifacts.load()) +
            " reused=" + std::to_string(first->reusedArtifacts.load()) +
            " completed=" +
            std::to_string(first->completedArtifacts.load()));
    }

    const auto failure = manager.request(
        {"failure", "profile", vkr::ImportReason::ManualReimport});
    requireImport(waitTerminal(failure), "failed import did not finish");
    {
        std::lock_guard lock(failure->mutex);
        requireImport(failure->state == vkr::AssetImportState::Failed &&
                          failure->processExitCode == 17 &&
                          failure->error == "synthetic importer failure",
                      "import failure details were not retained");
    }

    const auto cancelled = manager.request(
        {"cancel", "profile", vkr::ImportReason::ManualReimport});
    while (cancelled->state == vkr::AssetImportState::Queued)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    requireImport(manager.cancel(cancelled->id),
                  "active import could not be cancelled");
    requireImport(waitTerminal(cancelled), "cancelled import did not finish");
    requireImport(cancelled->state == vkr::AssetImportState::Cancelled,
                  "cancelled import has the wrong final state");

    const auto crash = manager.request(
        {"crash", "profile", vkr::ImportReason::ManualReimport});
    requireImport(waitTerminal(crash), "crashed import did not finish");
    {
        std::lock_guard lock(crash->mutex);
        requireImport(crash->state == vkr::AssetImportState::Failed &&
                          crash->error == "synthetic importer crash",
                      "importer crash was not converted to task failure");
    }
    const auto badProtocol = manager.request(
        {"bad-protocol", "profile", vkr::ImportReason::ManualReimport});
    requireImport(waitTerminal(badProtocol),
                  "invalid protocol import did not finish");
    {
        std::lock_guard lock(badProtocol->mutex);
        requireImport(
            badProtocol->state == vkr::AssetImportState::Failed &&
                badProtocol->error ==
                    "unsupported asset import progress protocol",
            "invalid progress protocol was not rejected");
    }
    requireImport(calls == 5, "manager executed an unexpected task count");
    manager.shutdown();
    std::filesystem::remove_all(root, ignored);
}

} // namespace

void runAssetImportManagerTests() { testManagerProgressMergeAndFailure(); }
