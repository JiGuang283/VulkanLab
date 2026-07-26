#pragma once

#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class DescriptorAllocator {
  public:
    explicit DescriptorAllocator(Device &device);
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator &) = delete;
    DescriptorAllocator &operator=(const DescriptorAllocator &) = delete;

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    VkDescriptorSet allocate(
        VkDescriptorSetLayout layout,
        std::initializer_list<VkDescriptorPoolSize> descriptorCounts,
        std::string_view debugName = {});
    void free(VkDescriptorSet set) noexcept;
    void            resetPools();

  private:
    struct PoolState {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t remainingSets = 0;
        std::unordered_map<VkDescriptorType, uint32_t> remainingDescriptors;
    };

    struct AllocationState {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPoolSize> descriptorCounts;
    };

    PoolState createPool();
    PoolState &currentPool(
        std::initializer_list<VkDescriptorPoolSize> descriptorCounts);
    bool canAllocate(const PoolState &pool,
                     std::initializer_list<VkDescriptorPoolSize>
                         descriptorCounts) const;
    void consume(PoolState &pool,
                 std::initializer_list<VkDescriptorPoolSize>
                     descriptorCounts) const;
    void resetState(PoolState &pool) const;

    Device                       *device_ = nullptr;
    std::vector<PoolState> usedPools_;
    std::vector<PoolState> freePools_;
    std::unordered_map<VkDescriptorSet, AllocationState> allocations_;
    uint32_t poolSerial_ = 0;
};

} // namespace vkr
