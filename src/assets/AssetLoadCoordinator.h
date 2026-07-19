#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vkr {

struct AssetLoadConsumer {
    uint64_t generation = 0;
    int sceneIndex = -1;
};

class AssetLoadCoordinator {
  public:
    uint64_t beginOperation();
    uint64_t currentGeneration() const { return generation_; }

    void attach(uint64_t importTaskId, uint64_t generation, int sceneIndex);
    std::optional<int> takeLatestScene(uint64_t importTaskId);
    void discard(uint64_t importTaskId);

  private:
    uint64_t generation_ = 0;
    std::unordered_map<uint64_t, std::vector<AssetLoadConsumer>> consumers_;
};

} // namespace vkr
