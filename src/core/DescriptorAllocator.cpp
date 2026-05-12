#include "DescriptorAllocator.h"

#include "Device.h"
#include "VulkanException.h"

#include <array>

namespace vkr {

DescriptorAllocator::DescriptorAllocator(Device &device) : device_(&device) {}

DescriptorAllocator::~DescriptorAllocator() {
    if (!device_)
        return;

    VkDevice device = device_->logicalDevice();
    for (VkDescriptorPool pool : usedPools_)
        vkDestroyDescriptorPool(device, pool, nullptr);
    for (VkDescriptorPool pool : freePools_)
        vkDestroyDescriptorPool(device, pool, nullptr);
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    allocInfo.descriptorPool = currentPool();
    VkResult result =
        vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo, &set);

    if (result == VK_SUCCESS)
        return set;

    if (result == VK_ERROR_OUT_OF_POOL_MEMORY ||
        result == VK_ERROR_FRAGMENTED_POOL) {
        usedPools_.push_back(createPool());
        allocInfo.descriptorPool = usedPools_.back();
        result = vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo,
                                          &set);
        if (result == VK_SUCCESS)
            return set;
    }

    throw VulkanException(result, "vkAllocateDescriptorSets", __FILE__,
                          __LINE__);
}

void DescriptorAllocator::resetPools() {
    VkDevice device = device_->logicalDevice();
    for (VkDescriptorPool pool : usedPools_) {
        vkResetDescriptorPool(device, pool, 0);
        freePools_.push_back(pool);
    }
    usedPools_.clear();
}

VkDescriptorPool DescriptorAllocator::createPool() {
    constexpr uint32_t                  maxSets = 128;
    std::array<VkDescriptorPoolSize, 4> sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult         result = vkCreateDescriptorPool(device_->logicalDevice(),
                                                     &poolInfo, nullptr, &pool);
    if (result != VK_SUCCESS)
        throw VulkanException(result, "vkCreateDescriptorPool", __FILE__,
                              __LINE__);
    return pool;
}

VkDescriptorPool DescriptorAllocator::currentPool() {
    if (!usedPools_.empty())
        return usedPools_.back();

    if (!freePools_.empty()) {
        VkDescriptorPool pool = freePools_.back();
        freePools_.pop_back();
        usedPools_.push_back(pool);
    } else {
        usedPools_.push_back(createPool());
    }
    return usedPools_.back();
}

} // namespace vkr