#include "GpuPassProfiler.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
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
                                 std::vector<GpuPassProfile> passes)
    : device_(&device), passes_(std::move(passes)) {
    if (passes_.empty() ||
        passes_.size() > std::numeric_limits<uint32_t>::max() / 2)
        return;
    for (uint32_t index = 0; index < passes_.size(); ++index) {
        if (passes_[index].id == 0 ||
            !passSlots_.emplace(passes_[index].id, index).second) {
            throw std::invalid_argument(
                "GPU profiler pass IDs must be non-zero and unique");
        }
    }

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

    queriesPerFrame_ = static_cast<uint32_t>(passes_.size()) * 2;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = queriesPerFrame_ * MAX_FRAMES_IN_FLIGHT;
    VK_CHECK(vkCreateQueryPool(device.logicalDevice(), &info, nullptr,
                               &queryPool_));
    device.debugUtils().setObjectName(VK_OBJECT_TYPE_QUERY_POOL, queryPool_,
                                      "Diagnostics/GpuPassTimestamps");
}

GpuPassProfiler::~GpuPassProfiler() {
    if (queryPool_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_->logicalDevice(), queryPool_, nullptr);
}

void GpuPassProfiler::collect(uint32_t frameIndex) {
    if (!supported() || frameIndex >= frameSlots_.size() ||
        !frameSlots_[frameIndex].recorded)
        return;

    FrameSlot &slot = frameSlots_[frameIndex];
    std::vector<std::array<uint64_t, 2>> values(passes_.size());
    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (passIndex >= slot.activePasses.size() ||
            slot.activePasses[passIndex] == 0)
            continue;
        const VkResult result = vkGetQueryPoolResults(
            device_->logicalDevice(), queryPool_,
            passQuery(frameIndex, passIndex, false), 2,
            sizeof(values[passIndex]), values[passIndex].data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_NOT_READY)
            return;
        VK_CHECK(result);
    }
    frameSlots_[frameIndex].recorded = false;

    GpuPassTimings timings{};
    timings.available = true;
    timings.frameSerial = frameSlots_[frameIndex].frameSerial;
    timings.passes.reserve(passes_.size());
    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (passIndex >= slot.activePasses.size() ||
            slot.activePasses[passIndex] == 0)
            continue;
        const uint64_t ticks = gpuTimestampDeltaTicks(
            values[passIndex][0], values[passIndex][1],
            timestampValidBits_);
        timings.passes.push_back(
            {passes_[passIndex].name, gpuTimestampTicksToMilliseconds(
                                        ticks, timestampPeriodNanoseconds_)});
    }
    if (slot.firstStartedPass != UINT32_MAX &&
        slot.lastCompletedPass != UINT32_MAX) {
        const uint64_t totalTicks = gpuTimestampDeltaTicks(
            values[slot.firstStartedPass][0],
            values[slot.lastCompletedPass][1],
            timestampValidBits_);
        timings.totalMs = gpuTimestampTicksToMilliseconds(
            totalTicks, timestampPeriodNanoseconds_);
    }
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
    FrameSlot &slot = frameSlots_[frameIndex];
    slot.recorded = true;
    slot.frameSerial = frameSerial;
    slot.activePasses.assign(passes_.size(), 0);
    slot.firstStartedPass = UINT32_MAX;
    slot.lastCompletedPass = UINT32_MAX;
}

void GpuPassProfiler::beginPass(VkCommandBuffer commandBuffer,
                                uint32_t frameIndex,
                                RenderGraphPassId passId) {
    if (!supported())
        return;
    if (frameIndex >= frameSlots_.size())
        throw std::out_of_range("GPU profiler query index is out of range");
    const uint32_t passSlot = passIndex(passId);
    FrameSlot &slot = frameSlots_[frameIndex];
    if (slot.firstStartedPass == UINT32_MAX)
        slot.firstStartedPass = passSlot;
    slot.activePasses[passSlot] = 1;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        queryPool_,
                        passQuery(frameIndex, passSlot, false));
}

void GpuPassProfiler::endPass(VkCommandBuffer commandBuffer,
                              uint32_t frameIndex,
                              RenderGraphPassId passId) {
    if (!supported())
        return;
    const uint32_t passSlot = passIndex(passId);
    frameSlots_.at(frameIndex).lastCompletedPass = passSlot;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        queryPool_, passQuery(frameIndex, passSlot, true));
}

uint32_t GpuPassProfiler::frameQueryBase(uint32_t frameIndex) const {
    return frameIndex * queriesPerFrame_;
}

uint32_t GpuPassProfiler::passQuery(uint32_t frameIndex,
                                    uint32_t passIndex, bool end) const {
    if (frameIndex >= frameSlots_.size() || passIndex >= passes_.size())
        throw std::out_of_range("GPU profiler query index is out of range");
    return frameQueryBase(frameIndex) + passIndex * 2 + (end ? 1u : 0u);
}

uint32_t GpuPassProfiler::passIndex(RenderGraphPassId passId) const {
    const auto it = passSlots_.find(passId);
    if (it == passSlots_.end())
        throw std::out_of_range("GPU profiler received an unknown pass ID");
    return it->second;
}

} // namespace vkr
