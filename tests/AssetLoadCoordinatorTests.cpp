#include "assets/AssetLoadCoordinator.h"

#include <stdexcept>

namespace {

void requireCoordinator(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testLatestGenerationWins() {
    vkr::AssetLoadCoordinator coordinator;
    const uint64_t first = coordinator.beginOperation();
    coordinator.attach(100, first, 1);
    const uint64_t second = coordinator.beginOperation();
    coordinator.attach(200, second, 2);

    requireCoordinator(!coordinator.takeLatestScene(100).has_value(),
                       "superseded import was allowed to publish a scene");
    const auto selected = coordinator.takeLatestScene(200);
    requireCoordinator(selected && *selected == 2,
                       "latest import did not retain its scene consumer");
}

void testMergedImportUsesNewestConsumer() {
    vkr::AssetLoadCoordinator coordinator;
    const uint64_t first = coordinator.beginOperation();
    coordinator.attach(100, first, 1);
    const uint64_t second = coordinator.beginOperation();
    coordinator.attach(100, second, 1);

    const auto selected = coordinator.takeLatestScene(100);
    requireCoordinator(selected && *selected == 1,
                       "merged import did not select its newest consumer");
    requireCoordinator(!coordinator.takeLatestScene(100).has_value(),
                       "completed import retained stale consumers");
}

} // namespace

void runAssetLoadCoordinatorTests() {
    testLatestGenerationWins();
    testMergedImportUsesNewestConsumer();
}
