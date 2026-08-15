#include "render/pass/OcclusionCullPass.h"

#include "core/Buffer.h"
#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kInitialCapacity = 256;
constexpr uint32_t kWorkgroupSize = 64;

struct alignas(16) GpuCullItem {
    glm::vec4 center{0.0f};
    glm::vec4 extent{0.0f};
    glm::uvec4 draw{0u};
};
static_assert(sizeof(GpuCullItem) == 48);

struct alignas(16) GpuVisibilityCounter {
    uint32_t visible = 0;
    uint32_t occluded = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};

struct alignas(16) OcclusionPush {
    glm::mat4 viewProjection{1.0f};
    glm::vec4 params{0.0f};
    glm::uvec4 counts{0u};
};
static_assert(sizeof(OcclusionPush) == 96);

uint32_t nextCapacity(uint32_t required) {
    uint32_t value = kInitialCapacity;
    while (value < required && value < OcclusionCullPass::kMaxCandidates)
        value *= 2u;
    return std::min(value, OcclusionCullPass::kMaxCandidates);
}

} // namespace

struct OcclusionCullPass::FrameStorage {
    std::unique_ptr<Buffer> items;
    std::unique_ptr<Buffer> indirect;
    std::unique_ptr<Buffer> counter;
    uint32_t capacity = 0;
    uint32_t activeCount = 0;
    uint64_t submittedSerial = 0;
    bool active = false;
    GpuVisibilityDrawStream stream{};
};

OcclusionCullPass::OcclusionCullPass(
    Device &device, const RenderResourceRegistry &resources,
    RendererResourceHandles resourceHandles,
    DescriptorAllocator &descriptorAllocator, std::string computeShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      computeShaderPath_(std::move(computeShaderPath)), resources_(&resources) {
    if (!resourceHandles_.visibilityHiZ.valid())
        throw std::invalid_argument("OcclusionCullPass requires Hi-Z image");
    createDescriptorSetLayout();
    for (auto &frame : frames_)
        frame = std::make_unique<FrameStorage>();
    createDescriptorSets(resources);
    for (uint32_t frame = 0; frame < frames_.size(); ++frame)
        ensureCapacity(frame, kInitialCapacity);
}

OcclusionCullPass::~OcclusionCullPass() {
    freeDescriptorSets();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void OcclusionCullPass::setup(RenderGraphBuilder &builder,
                              const RenderGraphBuildContext &) const {
    builder.addNode("OcclusionCull/ClearCounter", RgPassType::Transfer,
                    RgQueueClass::Transfer, 0);
    for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
        const FrameStorage &storage = *frames_[frame];
        builder.useBuffer(storage.counter->handle(),
                          RgBufferAccess::TransferWrite, 0,
                          sizeof(GpuVisibilityCounter), frame);
    }

    builder.addNode("OcclusionCull/Dispatch", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    builder.useImage({resourceHandles_.visibilityHiZ,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
        const FrameStorage &storage = *frames_[frame];
        builder.useBuffer(storage.items->handle(), RgBufferAccess::StorageRead,
                          0, VK_WHOLE_SIZE, frame);
        builder.useBuffer(storage.indirect->handle(),
                          RgBufferAccess::StorageWrite, 0, VK_WHOLE_SIZE,
                          frame);
        builder.useBuffer(storage.counter->handle(),
                          RgBufferAccess::StorageReadWrite, 0,
                          sizeof(GpuVisibilityCounter), frame);
    }

    builder.addNode("OcclusionCull/IndirectReady", RgPassType::External,
                    RgQueueClass::Graphics, 2);
    builder.setSideEffect();
    for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
        const FrameStorage &storage = *frames_[frame];
        builder.useBuffer(storage.indirect->handle(),
                          RgBufferAccess::IndirectRead, 0, VK_WHOLE_SIZE,
                          frame);
    }
}

void OcclusionCullPass::recordNode(RenderGraphPassContext &context,
                                   uint32_t localNodeIndex,
                                   const VisibilityFrame &) {
    FrameStorage &storage = *frames_.at(context.frame.frameIndex);
    if (!storage.active || storage.activeCount == 0)
        return;
    if (localNodeIndex == 0) {
        vkCmdFillBuffer(context.frame.cmd, storage.counter->handle(), 0,
                        sizeof(GpuVisibilityCounter), 0);
    } else if (localNodeIndex == 1) {
        recordCull(context.frame, context.resources);
    }
}

uint64_t OcclusionCullPass::topologySignature() const {
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](uint64_t value) {
        signature ^= value;
        signature *= 1099511628211ull;
    };
    for (const auto &frame : frames_) {
        mix(reinterpret_cast<uint64_t>(frame->items->handle()));
        mix(reinterpret_cast<uint64_t>(frame->indirect->handle()));
        mix(reinterpret_cast<uint64_t>(frame->counter->handle()));
        mix(frame->capacity);
    }
    return signature;
}

void OcclusionCullPass::releaseViewportResources() {
    freeDescriptorSets();
    resources_ = nullptr;
}

void OcclusionCullPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    resources_ = &resources;
    createDescriptorSets(resources);
    for (uint32_t frame = 0; frame < frames_.size(); ++frame)
        updateDescriptor(frame, resources);
}

void OcclusionCullPass::prepareFrame(uint32_t frameIndex,
                                     uint64_t frameSerial,
                                     const VisibilityFrame &visibility,
                                     const RenderView &view) {
    FrameStorage &storage = *frames_.at(frameIndex);
    if (storage.submittedSerial != 0 && storage.counter) {
        storage.counter->invalidate();
        const auto *counter = static_cast<const GpuVisibilityCounter *>(
            storage.counter->mappedData());
        completedStatistics_.frameSerial = storage.submittedSerial;
        completedStatistics_.candidates = storage.activeCount;
        completedStatistics_.visible = counter->visible;
        completedStatistics_.occluded = counter->occluded;
    }

    const uint32_t requested = static_cast<uint32_t>(
        visibility.cameraOpaque.size());
    const uint32_t activeCount =
        std::min(requested, kMaxCandidates);
    ensureCapacity(frameIndex, std::max(activeCount, 1u));
    storage.activeCount = activeCount;
    storage.active = view.settings.culling.occlusionEnabled &&
                     activeCount >=
                         view.settings.culling.occlusionMinCandidates;
    storage.submittedSerial = storage.active ? frameSerial : 0;
    storage.stream.indirectBuffer = storage.indirect
                                        ? storage.indirect->handle()
                                        : VK_NULL_HANDLE;
    storage.stream.candidateCount = activeCount;
    storage.stream.frameIndex = frameIndex;
    storage.stream.visibilityGeneration = visibility.generation;
    storage.stream.active = storage.active;

    auto *items = static_cast<GpuCullItem *>(storage.items->mappedData());
    for (uint32_t index = 0; index < activeCount; ++index) {
        const RenderItemIndex itemIndex = visibility.cameraOpaque[index];
        const RenderItem &command = visibility.items.at(itemIndex);
        const Bounds &bounds = command.worldBounds;
        GpuCullItem item{};
        if (bounds.valid) {
            item.center = glm::vec4((bounds.min + bounds.max) * 0.5f, 1.0f);
            item.extent = glm::vec4(
                glm::max((bounds.max - bounds.min) * 0.5f,
                         glm::vec3(0.0f)),
                0.0f);
        }
        item.draw.x = command.mesh ? command.mesh->indexCount() : 0u;
        item.draw.y = bounds.valid ? 0u : 1u;
        item.draw.z = itemIndex;
        items[index] = item;
    }
}

void OcclusionCullPass::recordCull(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources) {
    FrameStorage &storage = *frames_.at(frame.frameIndex);
    if (!storage.active || storage.activeCount == 0 || !frame.pipelineCache ||
        !frame.view)
        return;

    ComputePipelineConfig config{};
    config.debugName = "Pipeline/Visibility/OcclusionCull";
    config.computeShaderPath = computeShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(OcclusionPush)}};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = descriptorSets_.at(frame.frameIndex);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    const VkExtent2D extent =
        resources.extent(resourceHandles_.visibilityHiZ);
    OcclusionPush push{};
    push.viewProjection = frame.view->globalUbo.proj *
                          frame.view->globalUbo.view;
    push.params = glm::vec4(static_cast<float>(extent.width),
                            static_cast<float>(extent.height),
                            frame.view->settings.culling.occlusionDepthBias,
                            0.0f);
    push.counts = glm::uvec4(
        storage.activeCount,
        resources.mipLevelCount(resourceHandles_.visibilityHiZ), 0u, 0u);
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd,
                  (storage.activeCount + kWorkgroupSize - 1u) /
                      kWorkgroupSize,
                   1, 1);
}

bool OcclusionCullPass::active(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->active;
}

const GpuVisibilityDrawStream &
OcclusionCullPass::drawStream(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->stream;
}

uint32_t OcclusionCullPass::capacity(uint32_t frameIndex) const {
    return frames_.at(frameIndex)->capacity;
}

uint64_t OcclusionCullPass::allocatedBytes() const {
    uint64_t bytes = 0;
    for (const auto &frame : frames_) {
        bytes += static_cast<uint64_t>(frame->capacity) *
                 (sizeof(GpuCullItem) +
                  sizeof(VkDrawIndexedIndirectCommand));
        if (frame->counter)
            bytes += sizeof(GpuVisibilityCounter);
    }
    return bytes;
}

void OcclusionCullPass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/OcclusionCull");
}

void OcclusionCullPass::createDescriptorSets(
    const RenderResourceRegistry &resources) {
    (void)resources;
    for (uint32_t frame = 0; frame < descriptorSets_.size(); ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}},
            "Visibility/Occlusion/Frame" + std::to_string(frame));
    }
}

void OcclusionCullPass::freeDescriptorSets() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

void OcclusionCullPass::updateDescriptor(
    uint32_t frameIndex, const RenderResourceRegistry &resources) {
    const FrameStorage &storage = *frames_.at(frameIndex);
    if (!storage.items || !storage.indirect || !storage.counter)
        return;
    VkDescriptorImageInfo hiZ{};
    hiZ.sampler = resources.sampler(resourceHandles_.visibilityHiZSampler);
    hiZ.imageView =
        resources.image(resourceHandles_.visibilityHiZ, frameIndex)
            .imageView();
    hiZ.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkDescriptorBufferInfo items{storage.items->handle(), 0,
                                       VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo indirect{storage.indirect->handle(), 0,
                                          VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo counter{storage.counter->handle(), 0,
                                         sizeof(GpuVisibilityCounter)};
    std::array<VkWriteDescriptorSet, 4> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSets_[frameIndex];
    writes[0].dstBinding = 0;
    writes[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &hiZ;
    const std::array<VkDescriptorBufferInfo, 3> buffers{
        items, indirect, counter};
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        writes[index + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index + 1].dstSet = descriptorSets_[frameIndex];
        writes[index + 1].dstBinding = index + 1;
        writes[index + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index + 1].descriptorCount = 1;
        writes[index + 1].pBufferInfo = &buffers[index];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void OcclusionCullPass::ensureCapacity(uint32_t frameIndex,
                                       uint32_t required) {
    FrameStorage &storage = *frames_.at(frameIndex);
    if (storage.capacity >= required)
        return;
    const uint32_t capacityValue = nextCapacity(required);
    constexpr VmaAllocationCreateFlags hostFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    storage.items = std::make_unique<Buffer>(
        *device_, sizeof(GpuCullItem) * capacityValue,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        hostFlags,
        "Visibility/CullItems/Frame" + std::to_string(frameIndex));
    storage.items->map();
    storage.indirect = std::make_unique<Buffer>(
        *device_, sizeof(VkDrawIndexedIndirectCommand) * capacityValue,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
        "Visibility/Indirect/Frame" + std::to_string(frameIndex));
    storage.counter = std::make_unique<Buffer>(
        *device_, sizeof(GpuVisibilityCounter),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        hostFlags,
        "Visibility/Counter/Frame" + std::to_string(frameIndex));
    storage.counter->map();
    std::memset(storage.counter->mappedData(), 0,
                sizeof(GpuVisibilityCounter));
    storage.capacity = capacityValue;
    if (resources_)
        updateDescriptor(frameIndex, *resources_);
}

} // namespace vkr
