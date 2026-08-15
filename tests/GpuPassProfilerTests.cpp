#include "render/diagnostics/GpuPassProfiler.h"

#include <cmath>
#include <stdexcept>

namespace {

void requireGpuProfiler(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testTimestampDeltaWithoutWrap() {
    requireGpuProfiler(vkr::gpuTimestampDeltaTicks(10, 25, 64) == 15,
                       "64-bit timestamp delta is incorrect");
    requireGpuProfiler(vkr::gpuTimestampDeltaTicks(10, 25, 8) == 15,
                       "masked timestamp delta is incorrect");
}

void testTimestampDeltaWithWrap() {
    requireGpuProfiler(vkr::gpuTimestampDeltaTicks(250, 5, 8) == 11,
                       "timestamp counter wrap was not handled");
    requireGpuProfiler(vkr::gpuTimestampDeltaTicks(1, 2, 0) == 0,
                       "unsupported timestamp width produced a delta");
}

void testTimestampUnitConversion() {
    const double milliseconds =
        vkr::gpuTimestampTicksToMilliseconds(2'000'000, 0.5);
    requireGpuProfiler(std::abs(milliseconds - 1.0) < 1.0e-9,
                       "timestamp period conversion is incorrect");
}

} // namespace

void runGpuPassProfilerTests() {
    testTimestampDeltaWithoutWrap();
    testTimestampDeltaWithWrap();
    testTimestampUnitConversion();
}
