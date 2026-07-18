#pragma once

#include "UploadRecorder.h"

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class Device;
struct ResourceLoadStats;

class UploadContext : public UploadRecorder {
  public:
    static constexpr VkDeviceSize kDefaultStagingCapacity =
        128ull * 1024ull * 1024ull;
    static constexpr VkDeviceSize kInitialStagingCapacity =
        16ull * 1024ull * 1024ull;

    explicit UploadContext(
        Device &device, ResourceLoadStats *stats = nullptr,
        VkDeviceSize stagingCapacity = kDefaultStagingCapacity);
    ~UploadContext();

    UploadContext(const UploadContext &) = delete;
    UploadContext &operator=(const UploadContext &) = delete;

    StagedSlice stageBytes(const void *data, VkDeviceSize size) override;
    void uploadBuffer(const void *data, VkDeviceSize size, VkBuffer dst,
                      VkDeviceSize dstOffset = 0);
    VkCommandBuffer commandBuffer() override;
    void finish();

    ResourceLoadStats *stats() const override { return stats_; }

  private:
    static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment);

    void ensureStaging(VkDeviceSize capacity, bool allowShrink = false);
    void beginCommands();
    void flushAndWait();

    Device            *device_ = nullptr;
    ResourceLoadStats *stats_ = nullptr;
    VkDeviceSize       defaultCapacity_ = 0;
    VkDeviceSize       stagingCapacity_ = 0;
    VkDeviceSize       copyAlignment_ = 4;
    VkDeviceSize       cursor_ = 0;

    std::unique_ptr<Buffer> staging_;
    void                   *mapped_ = nullptr;
    VkCommandPool           commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer         commandBuffer_ = VK_NULL_HANDLE;
    VkFence                 fence_ = VK_NULL_HANDLE;
    bool                    recording_ = false;
    bool                    hasCommands_ = false;
    bool                    oversizedBatch_ = false;
};

} // namespace vkr
