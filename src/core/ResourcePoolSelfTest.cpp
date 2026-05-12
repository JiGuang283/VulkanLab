#include "ResourcePoolSelfTest.h"

#include "ResourcePool.h"

#include <stdexcept>

namespace vkr {

namespace {

struct SelfTestTag {};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

void runResourcePoolSelfTest() {
    ResourcePool<int, SelfTestTag> pool;

    auto first = pool.emplace(42);
    require(pool.alive(first),
            "ResourcePool self-test failed: inserted handle is not alive");
    require(pool.get(first) && *pool.get(first) == 42,
            "ResourcePool self-test failed: inserted value mismatch");

    pool.release(first);
    require(!pool.alive(first),
            "ResourcePool self-test failed: released handle is still alive");
    require(
        pool.get(first) == nullptr,
        "ResourcePool self-test failed: released handle still returns value");

    auto second = pool.emplace(7);
    require(second.index == first.index,
            "ResourcePool self-test failed: slot was not reused");
    require(second.generation != first.generation,
            "ResourcePool self-test failed: generation did not change");
    require(pool.alive(second),
            "ResourcePool self-test failed: reused handle is not alive");

    pool.clear();
    require(!pool.alive(second),
            "ResourcePool self-test failed: clear did not invalidate handle");
    require(pool.size() == 0,
            "ResourcePool self-test failed: clear did not reset live count");
}

} // namespace vkr