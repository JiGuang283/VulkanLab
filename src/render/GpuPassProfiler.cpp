#include "GpuPassProfiler.h"

#include "core/Device.h"
#include "core/VulkanCheck.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace vkr {

uint64_t gpuTimestampDeltaTicks(uint64_t begin, uint64_t end,
                                uint32_t validBits) {
    if (validBits == 0)
        return 0;
    if (validBits >= 64)
        return end - begin;
    const uint64_t mask = (uint64_t{1} << validBits) - 1;
    return ((end & mask) - (begin & mask)) & mask;
}

double gpuTimestampTicksToMilliseconds(
    uint64_t ticks, double timestampPeriodNanoseconds) {
    return static_cast<double>(ticks) * timestampPeriodNanoseconds /
           1'000'000.0;
}

GpuPassProfiler::GpuPassProfiler(Device &device,
                                 std::vector<std::string> passNames)
    : device_(&device), passNames_(std::move(passNames)) {
    if (passNames_.empty() ||
        passNames_.size() > std::numeric_limits<uint32_t>::max() / 2)
        return;

    const QueueFamilyIndices indices = device.queueFamilies();
    if (!indices.graphicsFamily)
        return;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(),
                                              &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(),
                                              &familyCount, families.data());
    if (*indices.graphicsFamily >= familyCount)
        return;

    timestampValidBits_ = families[*indices.graphicsFamily].timestampValidBits;
    timestampPeriodNanoseconds_ =
        device.physicalDeviceProperties().limits.timestampPeriod;
    if (timestampValidBits_ == 0 || timestampPeriodNanoseconds_ <= 0.0)
        return;

    queriesPerFrame_ = static_cast<uint32_t>(passNames_.size()) * 2;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = queriesPerFrame_ * MAX_FRAMES_IN_FLIGHT;
    VK_CHECK(vkCreateQueryPool(device.logicalDevice(), &info, nullptr,
                               &queryPool_));
}

GpuPassProfiler::~GpuPassProfiler() {
    if (queryPool_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_->logicalDevice(), queryPool_, nullptr);
}

void GpuPassProfiler::collect(uint32_t frameIndex) {
    if (!supported() || frameIndex >= frameSlots_.size() ||
        !frameSlots_[frameIndex].recorded)
        return;

    std::vector<uint64_t> values(queriesPerFrame_);
    const VkResult result = vkGetQueryPoolResults(
        device_->logicalDevice(), queryPool_, frameQueryBase(frameIndex),
        queriesPerFrame_, values.size() * sizeof(uint64_t), values.data(),
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    frameSlots_[frameIndex].recorded = false;
    if (result == VK_NOT_READY)
        return;
    VK_CHECK(result);

    GpuPassTimings timings{};
    timings.available = true;
    timings.frameSerial = frameSlots_[frameIndex].frameSerial;
    timings.passes.reserve(passNames_.size());
    for (uint32_t passIndex = 0; passIndex < passNames_.size(); ++passIndex) {
        const uint64_t ticks = gpuTimestampDeltaTicks(
            values[passIndex * 2], values[passIndex * 2 + 1],
            timestampValidBits_);
        timings.passes.push_back(
            {passNames_[passIndex], gpuTimestampTicksToMilliseconds(
                                        ticks, timestampPeriodNanoseconds_)});
    }
    const uint64_t totalTicks = gpuTimestampDeltaTicks(
        values.front(), values.back(), timestampValidBits_);
    timings.totalMs = gpuTimestampTicksToMilliseconds(
        totalTicks, timestampPeriodNanoseconds_);
    latest_ = std::move(timings);
}

void GpuPassProfiler::beginFrame(VkCommandBuffer commandBuffer,
                                 uint32_t frameIndex,
                                 uint64_t frameSerial) {
    if (!supported())
        return;
    if (frameIndex >= frameSlots_.size())
        throw std::out_of_range("GPU profiler frame index is out of range");
    vkCmdResetQueryPool(commandBuffer, queryPool_,
                        frameQueryBase(frameIndex), queriesPerFrame_);
    frameSlots_[frameIndex] = {true, frameSerial};
}

void GpuPassProfiler::beginPass(VkCommandBuffer commandBuffer,
                                uint32_t frameIndex,
                                uint32_t passIndex) const {
    if (!supported())
        return;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        queryPool_,
                        passQuery(frameIndex, passIndex, false));
}

void GpuPassProfiler::endPass(VkCommandBuffer commandBuffer,
                              uint32_t frameIndex,
                              uint32_t passIndex) const {
    if (!supported())
        return;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        queryPool_, passQuery(frameIndex, passIndex, true));
}

uint32_t GpuPassProfiler::frameQueryBase(uint32_t frameIndex) const {
    return frameIndex * queriesPerFrame_;
}

uint32_t GpuPassProfiler::passQuery(uint32_t frameIndex,
                                    uint32_t passIndex, bool end) const {
    if (frameIndex >= frameSlots_.size() || passIndex >= passNames_.size())
        throw std::out_of_range("GPU profiler query index is out of range");
    return frameQueryBase(frameIndex) + passIndex * 2 + (end ? 1u : 0u);
}

} // namespace vkr
