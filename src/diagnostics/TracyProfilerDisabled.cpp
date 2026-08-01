#include "TracyProfiler.h"

#include <utility>

namespace vkr {

struct TracyGpuZone::Impl {};
struct TracyProfiler::Impl {};

TracyGpuZone::TracyGpuZone() = default;
TracyGpuZone::TracyGpuZone(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
TracyGpuZone::~TracyGpuZone() = default;
TracyGpuZone::TracyGpuZone(TracyGpuZone &&) noexcept = default;
TracyGpuZone &TracyGpuZone::operator=(TracyGpuZone &&) noexcept = default;

TracyProfiler::TracyProfiler(VkInstance, VkPhysicalDevice, VkDevice,
                             VkQueue, uint32_t)
    : impl_(std::make_unique<Impl>()) {}
TracyProfiler::~TracyProfiler() = default;

bool TracyProfiler::compiled() const { return false; }
bool TracyProfiler::connected() const { return false; }
bool TracyProfiler::gpuAvailable() const { return false; }
std::string_view TracyProfiler::version() const { return {}; }
void TracyProfiler::collect(VkCommandBuffer) const {}
TracyGpuZone TracyProfiler::beginGpuZone(VkCommandBuffer,
                                          std::string_view) const {
    return {};
}

} // namespace vkr
