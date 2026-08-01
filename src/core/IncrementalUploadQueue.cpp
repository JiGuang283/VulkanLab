#include "IncrementalUploadQueue.h"

#include "Buffer.h"
#include "Device.h"
#include "GpuDebugUtils.h"
#include "UploadRecorder.h"
#include "VulkanCheck.h"
#include "diagnostics/SceneLoadStats.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vkr {

namespace {

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0)
        return value;
    const VkDeviceSize increment = alignment - remainder;
    if (value > std::numeric_limits<VkDeviceSize>::max() - increment)
        throw std::overflow_error("Upload staging offset overflow");
    return value + increment;
}

} // namespace

class IncrementalUploadQueue::Slot final : public UploadRecorder {
  public:
    Slot(Device &device, ResourceLoadStats *stats,
         VkDeviceSize defaultCapacity, uint32_t slotIndex,
         std::string debugPrefix)
        : device_(&device), stats_(stats),
          defaultCapacity_(defaultCapacity), slotIndex_(slotIndex),
          debugPrefix_(std::move(debugPrefix)) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
        alignment_ = std::max<VkDeviceSize>(
            16, properties.limits.optimalBufferCopyOffsetAlignment);

        const QueueFamilyIndices families = device.queueFamilies();
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = families.graphicsFamily.value();
        VK_CHECK(vkCreateCommandPool(device.logicalDevice(), &poolInfo,
                                     nullptr, &commandPool_));
        device.debugUtils().setObjectName(
            VK_OBJECT_TYPE_COMMAND_POOL, commandPool_,
            debugPrefix_ + "/Slot" + std::to_string(slotIndex_) +
                "/CommandPool");

        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device.logicalDevice(),
                                          &commandInfo, &commandBuffer_));
        device.debugUtils().setObjectName(
            VK_OBJECT_TYPE_COMMAND_BUFFER, commandBuffer_,
            debugPrefix_ + "/Slot" + std::to_string(slotIndex_) +
                "/CommandBuffer");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(device.logicalDevice(), &fenceInfo, nullptr,
                               &fence_));
        device.debugUtils().setObjectName(
            VK_OBJECT_TYPE_FENCE, fence_,
            debugPrefix_ + "/Slot" + std::to_string(slotIndex_) + "/Fence");
    }

    ~Slot() override {
        tracyZone_ = {};
        if (mapped_)
            staging_->unmap();
        staging_.reset();
        if (fence_ != VK_NULL_HANDLE)
            vkDestroyFence(device_->logicalDevice(), fence_, nullptr);
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_->logicalDevice(), commandPool_,
                                 nullptr);
        }
    }

    bool canFit(VkDeviceSize required) const {
        if (inFlight_)
            return false;
        if (!staging_)
            return true;
        const VkDeviceSize offset = alignUp(cursor_, alignment_);
        return offset <= capacity_ && required <= capacity_ - offset;
    }

    void prepare(VkDeviceSize required) {
        if (inFlight_)
            throw std::logic_error("Cannot prepare an in-flight upload slot");
        const VkDeviceSize offset = alignUp(cursor_, alignment_);
        if (staging_ && offset <= capacity_ &&
            required <= capacity_ - offset) {
            return;
        }
        if (hasCommands_)
            throw std::logic_error("Active upload slot has insufficient space");

        const VkDeviceSize capacity =
            std::max(defaultCapacity_, alignUp(required, alignment_));
        replaceStaging(capacity);
    }

    StagedSlice stageBytes(const void *data, VkDeviceSize size) override {
        if (!data || size == 0)
            throw std::invalid_argument("Cannot stage empty upload data");
        const VkDeviceSize offset = alignUp(cursor_, alignment_);
        if (!staging_ || offset > capacity_ || size > capacity_ - offset)
            throw std::runtime_error("Upload slot reservation exceeded");
        std::memcpy(static_cast<uint8_t *>(mapped_) + offset, data,
                    static_cast<size_t>(size));
        cursor_ = offset + size;
        if (stats_) {
            stats_->peakStagingBytes = std::max<uint64_t>(
                stats_->peakStagingBytes,
                static_cast<uint64_t>(cursor_));
        }
        return {staging_->handle(), offset, size};
    }

    VkCommandBuffer commandBuffer() override {
        if (!recording_) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(commandBuffer_, &beginInfo));
            tracyZone_ = device_->tracyProfiler().beginGpuZone(
                commandBuffer_, batchLabel_, __LINE__, __FILE__, __func__);
            labelActive_ = device_->debugUtils().beginLabel(
                commandBuffer_, batchLabel_);
            recording_ = true;
        }
        hasCommands_ = true;
        return commandBuffer_;
    }

    ResourceLoadStats *stats() const override { return stats_; }

    bool hasCommands() const { return hasCommands_; }
    bool inFlight() const { return inFlight_; }
    VkDeviceSize stagedBytes() const { return cursor_; }
    void setBatchLabel(std::string label) {
        if (hasCommands_ || recording_ || inFlight_)
            throw std::logic_error("Cannot relabel an active upload slot");
        batchLabel_ = std::move(label);
    }

    void submit() {
        if (!hasCommands_)
            return;
        VKL_PROFILE_ZONE("Scene Upload Submit");
        if (labelActive_) {
            device_->debugUtils().endLabel(commandBuffer_);
            labelActive_ = false;
        }
        tracyZone_ = {};
        VK_CHECK(vkEndCommandBuffer(commandBuffer_));
        recording_ = false;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;
        VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo,
                               fence_));
        inFlight_ = true;
        if (stats_)
            ++stats_->batchSubmits;
    }

    bool poll() {
        if (!inFlight_)
            return false;
        VKL_PROFILE_ZONE("Scene Upload Fence Poll");
        if (stats_)
            ++stats_->fencePollCalls;
        const VkResult result =
            vkGetFenceStatus(device_->logicalDevice(), fence_);
        if (result == VK_NOT_READY)
            return false;
        VK_CHECK(result);
        if (stats_)
            ++stats_->completedBatchSubmits;
        resetAfterCompletion();
        return true;
    }

    void wait() {
        if (!inFlight_)
            return;
        VKL_PROFILE_ZONE("Scene Upload Fence Wait");
        const auto start = std::chrono::steady_clock::now();
        VK_CHECK(vkWaitForFences(device_->logicalDevice(), 1, &fence_,
                                 VK_TRUE, UINT64_MAX));
        if (stats_) {
            ++stats_->fenceWaitCalls;
            ++stats_->completedBatchSubmits;
            stats_->batchSubmitWaitMs +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
        }
        resetAfterCompletion();
    }

  private:
    void replaceStaging(VkDeviceSize capacity) {
        if (mapped_) {
            staging_->unmap();
            mapped_ = nullptr;
        }
        staging_.reset();
        staging_ = std::make_unique<Buffer>(
            *device_, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            debugPrefix_ + "/Slot" + std::to_string(slotIndex_) +
                "/StagingBuffer");
        mapped_ = staging_->map();
        capacity_ = capacity;
        cursor_ = 0;
        batchLabel_.clear();
    }

    void resetAfterCompletion() {
        VK_CHECK(vkResetFences(device_->logicalDevice(), 1, &fence_));
        VK_CHECK(vkResetCommandPool(device_->logicalDevice(), commandPool_,
                                    0));
        inFlight_ = false;
        hasCommands_ = false;
        recording_ = false;
        cursor_ = 0;
    }

    Device *device_ = nullptr;
    ResourceLoadStats *stats_ = nullptr;
    VkDeviceSize defaultCapacity_ = 0;
    VkDeviceSize alignment_ = 4;
    VkDeviceSize capacity_ = 0;
    VkDeviceSize cursor_ = 0;
    std::unique_ptr<Buffer> staging_;
    void *mapped_ = nullptr;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool recording_ = false;
    bool hasCommands_ = false;
    bool inFlight_ = false;
    bool labelActive_ = false;
    uint32_t slotIndex_ = 0;
    std::string debugPrefix_;
    std::string batchLabel_;
    TracyGpuZone tracyZone_;
};

IncrementalUploadQueue::IncrementalUploadQueue(
    Device &device, ResourceLoadStats *stats, uint32_t slotCount,
    VkDeviceSize slotCapacity, uint64_t taskId, std::string sceneName,
    std::string uploadLabel, std::string debugRoot)
    : device_(&device), stats_(stats), defaultCapacity_(slotCapacity),
      taskId_(taskId),
      sceneName_(sceneName.empty() ? "Unknown" : std::move(sceneName)),
      uploadLabel_(uploadLabel.empty()
                       ? "SceneUpload task=" + std::to_string(taskId)
                       : std::move(uploadLabel)) {
    if (slotCount < 2 || slotCapacity == 0)
        throw std::invalid_argument("Incremental upload queue requires slots");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
    copyAlignment_ = std::max<VkDeviceSize>(
        16, properties.limits.optimalBufferCopyOffsetAlignment);
    slots_.reserve(slotCount);
    if (debugRoot.empty())
        debugRoot = "SceneUpload/" + sceneName_ + "/Task" +
                    std::to_string(taskId_);
    for (uint32_t i = 0; i < slotCount; ++i) {
        slots_.push_back(std::make_unique<Slot>(
            device, stats, slotCapacity, i, debugRoot));
    }
}

IncrementalUploadQueue::~IncrementalUploadQueue() { drain(); }

UploadRecorder *IncrementalUploadQueue::acquire(
    VkDeviceSize requiredBytes) {
    if (requiredBytes == 0)
        throw std::invalid_argument("Upload resource size is zero");
    poll();
    if (active_ && active_->canFit(requiredBytes)) {
        active_->prepare(requiredBytes);
        return active_;
    }
    if (active_)
        submitActive();
    for (auto &slot : slots_) {
        if (!slot->inFlight() && !slot->hasCommands()) {
            slot->prepare(requiredBytes);
            slot->setBatchLabel(
                uploadLabel_ + " batch=" +
                std::to_string(nextBatchIndex_++));
            active_ = slot.get();
            return active_;
        }
    }
    return nullptr;
}

void IncrementalUploadQueue::submitActive() {
    if (!active_)
        return;
    active_->submit();
    active_ = nullptr;
    if (stats_) {
        stats_->peakInFlightBatches = std::max<uint64_t>(
            stats_->peakInFlightBatches, inFlightCount());
    }
}

void IncrementalUploadQueue::poll() {
    for (auto &slot : slots_)
        slot->poll();
}

bool IncrementalUploadQueue::idle() const {
    if (active_ && active_->hasCommands())
        return false;
    for (const auto &slot : slots_) {
        if (slot->inFlight() || slot->hasCommands())
            return false;
    }
    return true;
}

uint32_t IncrementalUploadQueue::inFlightCount() const {
    uint32_t count = 0;
    for (const auto &slot : slots_)
        count += slot->inFlight() ? 1u : 0u;
    return count;
}

VkDeviceSize IncrementalUploadQueue::stagingBytesInUse() const {
    VkDeviceSize bytes = 0;
    for (const auto &slot : slots_)
        bytes += slot->stagedBytes();
    return bytes;
}

void IncrementalUploadQueue::drain() {
    submitActive();
    for (auto &slot : slots_)
        slot->wait();
}

} // namespace vkr
