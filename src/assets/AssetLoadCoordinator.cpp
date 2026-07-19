#include "AssetLoadCoordinator.h"

namespace vkr {

uint64_t AssetLoadCoordinator::beginOperation() { return ++generation_; }

void AssetLoadCoordinator::attach(uint64_t importTaskId, uint64_t generation,
                                  int sceneIndex) {
    consumers_[importTaskId].push_back({generation, sceneIndex});
}

std::optional<int>
AssetLoadCoordinator::takeLatestScene(uint64_t importTaskId) {
    const auto found = consumers_.find(importTaskId);
    if (found == consumers_.end())
        return std::nullopt;
    std::optional<int> result;
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
        if (it->generation == generation_) {
            result = it->sceneIndex;
            break;
        }
    }
    consumers_.erase(found);
    return result;
}

void AssetLoadCoordinator::discard(uint64_t importTaskId) {
    consumers_.erase(importTaskId);
}

} // namespace vkr
