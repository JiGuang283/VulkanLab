#include "AssetImportManager.h"

#include <stdexcept>
#include <utility>

namespace vkr {

const char *assetImportStateName(AssetImportState state) {
    switch (state) {
    case AssetImportState::Queued:
        return "Queued";
    case AssetImportState::Scanning:
        return "Scanning";
    case AssetImportState::Importing:
        return "Importing";
    case AssetImportState::Publishing:
        return "Publishing";
    case AssetImportState::Completed:
        return "Completed";
    case AssetImportState::Failed:
        return "Failed";
    case AssetImportState::Cancelling:
        return "Cancelling";
    case AssetImportState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

const char *assetImportKindName(AssetImportKind kind) {
    switch (kind) {
    case AssetImportKind::SceneTextures:
        return "SceneTextures";
    case AssetImportKind::Environment:
        return "Environment";
    case AssetImportKind::SceneValidation:
        return "SceneValidation";
    }
    return "Unknown";
}

bool isTerminalAssetImportState(AssetImportState state) {
    return state == AssetImportState::Completed ||
           state == AssetImportState::Failed ||
           state == AssetImportState::Cancelled;
}

AssetImportManager::AssetImportManager(AssetImportManagerOptions options,
                                       AssetImportExecutor executor)
    : options_(std::move(options)), executor_(std::move(executor)) {}

AssetImportManager::~AssetImportManager() = default;

std::shared_ptr<AssetImportTask>
AssetImportManager::request(const AssetImportRequest &) {
    throw std::runtime_error("asset authoring support was not compiled");
}

bool AssetImportManager::cancel(uint64_t) { return false; }

std::shared_ptr<AssetImportTask>
AssetImportManager::task(uint64_t) const {
    return {};
}

std::shared_ptr<AssetImportTask> AssetImportManager::activeTask() const {
    return {};
}

std::vector<std::shared_ptr<AssetImportTask>>
AssetImportManager::history() const {
    return {};
}

void AssetImportManager::shutdown() {}

void AssetImportManager::workerLoop() {}

void AssetImportManager::applyEvent(
    const std::shared_ptr<AssetImportTask> &, const nlohmann::json &) {}

void AssetImportManager::pruneHistoryLocked() {}

AssetImportExecutionResult runAssetImportProcess(
    const AssetImportManagerOptions &, const AssetImportRequest &,
    const std::atomic_bool &, const AssetImportEventCallback &,
    const AssetImportLogCallback &) {
    return {1, false, "asset authoring support was not compiled"};
}

} // namespace vkr
