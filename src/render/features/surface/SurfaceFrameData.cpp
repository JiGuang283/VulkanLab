#include "render/features/surface/SurfaceFrameData.h"

#include "core/Buffer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/VulkanCheck.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "render/frame/FrameGpuData.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace vkr {
namespace {

constexpr uint32_t kInitialHistoryCapacity = 256;

uint32_t nextHistoryCapacity(uint32_t required) {
    uint32_t capacity = kInitialHistoryCapacity;
    while (capacity < required)
        capacity *= 2u;
    return capacity;
}

} // namespace

struct SurfaceFrameData::FrameStorage {
    std::unique_ptr<Buffer> frameUbo;
    std::unique_ptr<Buffer> history;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t historyCapacity = 0;
    uint64_t preparedVisibilityGeneration = 0;
};

SurfaceFrameData::SurfaceFrameData(
    Device &device, DescriptorAllocator &descriptorAllocator)
    : device_(&device), descriptorAllocator_(&descriptorAllocator) {
    createDescriptorSetLayout();
    createFrameStorage();
}

SurfaceFrameData::~SurfaceFrameData() {
    for (auto &frame : frames_) {
        if (frame && frame->descriptorSet != VK_NULL_HANDLE)
            descriptorAllocator_->free(frame->descriptorSet);
    }
    for (auto &frame : frames_)
        frame.reset();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void SurfaceFrameData::prepare(uint32_t frameIndex,
                               const VisibilityFrame &visibility,
                               VkExtent2D extent) {
    FrameStorage &storage = *frames_.at(frameIndex);
    if (storage.preparedVisibilityGeneration == visibility.generation)
        return;

    ensureHistoryCapacity(
        frameIndex,
        std::max(1u, static_cast<uint32_t>(visibility.items.size())));

    SurfaceFrameUbo frameUbo{};
    frameUbo.previousViewProjection =
        visibility.history.previousViewProjection;
    frameUbo.viewportSizeInvSize = {
        static_cast<float>(extent.width), static_cast<float>(extent.height),
        extent.width > 0 ? 1.0f / static_cast<float>(extent.width) : 0.0f,
        extent.height > 0 ? 1.0f / static_cast<float>(extent.height) : 0.0f};
    frameUbo.params.x = visibility.history.globalValid ? 1u : 0u;
    frameUbo.params.y =
        static_cast<uint32_t>(visibility.history.historyGeneration);
    std::memcpy(storage.frameUbo->mappedData(), &frameUbo,
                sizeof(frameUbo));

    auto *history = static_cast<GpuRenderItemHistory *>(
        storage.history->mappedData());
    for (uint32_t index = 0; index < visibility.items.size(); ++index) {
        history[index].previousWorld = visibility.items[index].previousWorld;
        history[index].params =
            glm::uvec4(visibility.items[index].historyValid ? 1u : 0u,
                       0u, 0u, 0u);
    }
    storage.preparedVisibilityGeneration = visibility.generation;
}

VkDescriptorSet SurfaceFrameData::descriptorSet(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->descriptorSet;
}

uint32_t SurfaceFrameData::historyCapacity(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->historyCapacity;
}

uint64_t SurfaceFrameData::allocatedBytes() const {
    uint64_t bytes = 0;
    for (const auto &frame : frames_) {
        bytes += sizeof(SurfaceFrameUbo);
        bytes += static_cast<uint64_t>(frame->historyCapacity) *
                 sizeof(GpuRenderItemHistory);
    }
    return bytes;
}

void SurfaceFrameData::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/SurfaceFrame");
}

void SurfaceFrameData::createFrameStorage() {
    constexpr VmaAllocationCreateFlags mappedFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    for (uint32_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
        auto storage = std::make_unique<FrameStorage>();
        storage->frameUbo = std::make_unique<Buffer>(
            *device_, sizeof(SurfaceFrameUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mappedFlags,
            "Frame/" + std::to_string(frameIndex) + "/SurfaceFrameUbo");
        storage->frameUbo->map();
        storage->historyCapacity = kInitialHistoryCapacity;
        storage->history = std::make_unique<Buffer>(
            *device_, sizeof(GpuRenderItemHistory) *
                          storage->historyCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mappedFlags,
            "Frame/" + std::to_string(frameIndex) +
                "/SurfaceHistory/Capacity" +
                std::to_string(storage->historyCapacity));
        storage->history->map();
        storage->descriptorSet = descriptorAllocator_->allocate(
            descriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
            "Frame/" + std::to_string(frameIndex) +
                "/SurfaceDescriptorSet");
        frames_[frameIndex] = std::move(storage);
        updateDescriptor(frameIndex);
    }
}

void SurfaceFrameData::updateDescriptor(uint32_t frameIndex) {
    const FrameStorage &storage = *frames_.at(frameIndex);
    const VkDescriptorBufferInfo uboInfo{
        storage.frameUbo->handle(), 0, sizeof(SurfaceFrameUbo)};
    const VkDescriptorBufferInfo historyInfo{
        storage.history->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = storage.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &uboInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = storage.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &historyInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void SurfaceFrameData::ensureHistoryCapacity(uint32_t frameIndex,
                                             uint32_t required) {
    FrameStorage &storage = *frames_.at(frameIndex);
    if (required <= storage.historyCapacity)
        return;
    const uint32_t capacity = nextHistoryCapacity(required);
    constexpr VmaAllocationCreateFlags mappedFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    storage.history = std::make_unique<Buffer>(
        *device_, sizeof(GpuRenderItemHistory) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        mappedFlags,
        "Frame/" + std::to_string(frameIndex) +
            "/SurfaceHistory/Capacity" + std::to_string(capacity));
    storage.history->map();
    storage.historyCapacity = capacity;
    updateDescriptor(frameIndex);
}

} // namespace vkr
