#include "GpuPassProfiler.h"

#include <limits>
#include <utility>

namespace vkr {

uint64_t gpuTimestampDeltaTicks(uint64_t begin, uint64_t end,
                                uint32_t validBits) {
    if (validBits == 0)
        return 0;
    if (validBits >= 64)
        return end - begin;
    const uint64_t mask = (uint64_t{1} << validBits) - 1;
    return (end - begin) & mask;
}

double gpuTimestampTicksToMilliseconds(
    uint64_t ticks, double timestampPeriodNanoseconds) {
    return static_cast<double>(ticks) * timestampPeriodNanoseconds /
           1'000'000.0;
}

GpuPassProfiler::GpuPassProfiler(Device &,
                                 std::vector<std::string> passNames)
    : passNames_(std::move(passNames)) {}

GpuPassProfiler::~GpuPassProfiler() = default;

void GpuPassProfiler::collect(uint32_t) {}

void GpuPassProfiler::beginFrame(VkCommandBuffer, uint32_t, uint64_t) {
    latest_.available = false;
}

void GpuPassProfiler::beginPass(VkCommandBuffer, uint32_t, uint32_t) const {}

void GpuPassProfiler::endPass(VkCommandBuffer, uint32_t, uint32_t) const {}

uint32_t GpuPassProfiler::frameQueryBase(uint32_t) const { return 0; }

uint32_t GpuPassProfiler::passQuery(uint32_t, uint32_t, bool) const {
    return 0;
}

} // namespace vkr
