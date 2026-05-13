#include "DescriptorAllocator.h"

#include "Device.h"
#include "VulkanException.h"

#include <array>
#include <utility>

namespace vkr {

namespace {
constexpr uint32_t kMaxSetsPerPool = 512;

constexpr std::array<VkDescriptorPoolSize, 4> kPoolSizes = {{
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 128},
}};
} // namespace

DescriptorAllocator::DescriptorAllocator(Device &device) : device_(&device) {}

DescriptorAllocator::~DescriptorAllocator() {
    if (!device_)
        return;

    VkDevice device = device_->logicalDevice();
    for (const PoolState &pool : usedPools_)
        vkDestroyDescriptorPool(device, pool.pool, nullptr);
    for (const PoolState &pool : freePools_)
        vkDestroyDescriptorPool(device, pool.pool, nullptr);
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    return allocate(layout, {});
}

VkDescriptorSet DescriptorAllocator::allocate(
    VkDescriptorSetLayout layout,
    std::initializer_list<VkDescriptorPoolSize> descriptorCounts) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    PoolState      *pool = &currentPool(descriptorCounts);
    allocInfo.descriptorPool = pool->pool;
    VkResult result =
        vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo, &set);

    if (result == VK_SUCCESS) {
        consume(*pool, descriptorCounts);
        return set;
    }

    if (result == VK_ERROR_OUT_OF_POOL_MEMORY ||
        result == VK_ERROR_FRAGMENTED_POOL) {
        usedPools_.push_back(createPool());
        pool = &usedPools_.back();
        allocInfo.descriptorPool = pool->pool;
        result = vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo,
                                          &set);
        if (result == VK_SUCCESS) {
            consume(*pool, descriptorCounts);
            return set;
        }
    }

    throw VulkanException(result, "vkAllocateDescriptorSets", __FILE__,
                          __LINE__);
}

void DescriptorAllocator::resetPools() {
    VkDevice device = device_->logicalDevice();
    for (PoolState &pool : usedPools_) {
        vkResetDescriptorPool(device, pool.pool, 0);
        resetState(pool);
        freePools_.push_back(std::move(pool));
    }
    usedPools_.clear();
}

DescriptorAllocator::PoolState DescriptorAllocator::createPool() {
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSetsPerPool;
    poolInfo.poolSizeCount = static_cast<uint32_t>(kPoolSizes.size());
    poolInfo.pPoolSizes = kPoolSizes.data();

    PoolState        pool{};
    VkResult         result = vkCreateDescriptorPool(device_->logicalDevice(),
                                                     &poolInfo, nullptr,
                                                     &pool.pool);
    if (result != VK_SUCCESS)
        throw VulkanException(result, "vkCreateDescriptorPool", __FILE__,
                              __LINE__);
    resetState(pool);
    return pool;
}

DescriptorAllocator::PoolState &DescriptorAllocator::currentPool(
    std::initializer_list<VkDescriptorPoolSize> descriptorCounts) {
    if (!usedPools_.empty() && canAllocate(usedPools_.back(), descriptorCounts))
        return usedPools_.back();

    for (auto it = freePools_.begin(); it != freePools_.end(); ++it) {
        if (!canAllocate(*it, descriptorCounts))
            continue;
        usedPools_.push_back(std::move(*it));
        freePools_.erase(it);
        return usedPools_.back();
    }

    usedPools_.push_back(createPool());
    return usedPools_.back();
}

bool DescriptorAllocator::canAllocate(
    const PoolState &pool,
    std::initializer_list<VkDescriptorPoolSize> descriptorCounts) const {
    if (pool.remainingSets == 0)
        return false;

    for (const auto &required : descriptorCounts) {
        auto it = pool.remainingDescriptors.find(required.type);
        if (it == pool.remainingDescriptors.end() ||
            it->second < required.descriptorCount) {
            return false;
        }
    }
    return true;
}

void DescriptorAllocator::consume(
    PoolState &pool,
    std::initializer_list<VkDescriptorPoolSize> descriptorCounts) const {
    --pool.remainingSets;
    for (const auto &required : descriptorCounts)
        pool.remainingDescriptors[required.type] -= required.descriptorCount;
}

void DescriptorAllocator::resetState(PoolState &pool) const {
    pool.remainingSets = kMaxSetsPerPool;
    pool.remainingDescriptors.clear();
    for (const auto &size : kPoolSizes)
        pool.remainingDescriptors[size.type] = size.descriptorCount;
}

} // namespace vkr
