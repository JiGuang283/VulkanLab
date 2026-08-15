#include "render/features/shadows_visibility/PunctualShadowSliceBuffer.h"

#include "core/Buffer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/VulkanCheck.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace vkr {
namespace {

VkDeviceSize alignedSize(VkDeviceSize size, VkDeviceSize alignment) {
    return alignment > 0 ? (size + alignment - 1) & ~(alignment - 1)
                         : size;
}

} // namespace

PunctualShadowSliceBuffer::PunctualShadowSliceBuffer(
    Device &device, DescriptorAllocator &descriptorAllocator,
    uint32_t sliceCount, VkShaderStageFlags stageFlags,
    std::string debugName)
    : device_(&device), sliceCount_(sliceCount) {
    if (sliceCount_ == 0)
        throw std::invalid_argument("punctual shadow slice count is zero");
    stride_ = alignedSize(
        sizeof(PunctualShadowSlice),
        device.physicalDeviceProperties()
            .limits.minUniformBufferOffsetAlignment);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    binding.descriptorCount = 1;
    binding.stageFlags = stageFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device.logicalDevice(),
                                         &layoutInfo, nullptr, &layout_));
    device.debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout_,
        "DescriptorLayout/" + debugName);

    constexpr VmaAllocationCreateFlags allocationFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        buffers_[frame] = std::make_unique<Buffer>(
            device, stride_ * sliceCount_,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            allocationFlags,
            "Frame/" + std::to_string(frame) + "/" + debugName);
        buffers_[frame]->map();
        descriptorSets_[frame] = descriptorAllocator.allocate(
            layout_, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}},
            debugName + "/Frame" + std::to_string(frame));
        const VkDescriptorBufferInfo bufferInfo{
            buffers_[frame]->handle(), 0, sizeof(PunctualShadowSlice)};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets_[frame];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device.logicalDevice(), 1, &write, 0,
                               nullptr);
    }
}

PunctualShadowSliceBuffer::~PunctualShadowSliceBuffer() {
    for (auto &buffer : buffers_)
        buffer.reset();
    if (layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(), layout_,
                                     nullptr);
}

void PunctualShadowSliceBuffer::write(
    uint32_t frameIndex, uint32_t sliceIndex,
    const PunctualShadowSlice &slice) {
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || sliceIndex >= sliceCount_)
        throw std::out_of_range("punctual shadow slice index");
    auto *mapped =
        static_cast<std::byte *>(buffers_[frameIndex]->mappedData());
    std::memcpy(mapped + stride_ * sliceIndex, &slice, sizeof(slice));
}

uint32_t PunctualShadowSliceBuffer::dynamicOffset(
    uint32_t sliceIndex) const {
    if (sliceIndex >= sliceCount_)
        throw std::out_of_range("punctual shadow slice index");
    return static_cast<uint32_t>(stride_ * sliceIndex);
}

} // namespace vkr
