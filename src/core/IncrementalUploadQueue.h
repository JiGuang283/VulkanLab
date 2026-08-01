#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class UploadRecorder;
struct ResourceLoadStats;

class IncrementalUploadQueue {
  public:
    static constexpr VkDeviceSize kDefaultSlotCapacity =
        128ull * 1024ull * 1024ull;

    explicit IncrementalUploadQueue(
        Device &device, ResourceLoadStats *stats = nullptr,
        uint32_t slotCount = 2,
        VkDeviceSize slotCapacity = kDefaultSlotCapacity,
        uint64_t taskId = 0, std::string sceneName = {});
    ~IncrementalUploadQueue();

    IncrementalUploadQueue(const IncrementalUploadQueue &) = delete;
    IncrementalUploadQueue &operator=(const IncrementalUploadQueue &) = delete;

    UploadRecorder *acquire(VkDeviceSize requiredBytes);
    void submitActive();
    void poll();
    bool idle() const;
    uint32_t inFlightCount() const;
    VkDeviceSize stagingBytesInUse() const;
    VkDeviceSize copyAlignment() const { return copyAlignment_; }
    void drain();

  private:
    class Slot;

    Device *device_ = nullptr;
    ResourceLoadStats *stats_ = nullptr;
    VkDeviceSize defaultCapacity_ = 0;
    VkDeviceSize copyAlignment_ = 4;
    std::vector<std::unique_ptr<Slot>> slots_;
    Slot *active_ = nullptr;
    uint64_t taskId_ = 0;
    uint64_t nextBatchIndex_ = 0;
    std::string sceneName_;
};

} // namespace vkr
