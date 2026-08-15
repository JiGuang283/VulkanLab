#include "RenderGraph.h"

#include "GpuPassProfiler.h"
#include "RenderFrame.h"
#include "RenderResourceRegistry.h"
#include "Visibility.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/SwapChain.h"
#include "render/pass/IRenderPass.h"

#include <algorithm>
#include <iomanip>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace vkr {
namespace {

RenderGraphPassId groupPassId(std::string_view name,
                              uint32_t ownerPassIndex) {
    uint64_t id = 1469598103934665603ull;
    for (const unsigned char c : name) {
        id ^= c;
        id *= 1099511628211ull;
    }
    id ^= static_cast<uint64_t>(ownerPassIndex) +
          0x9e3779b97f4a7c15ull + (id << 6u) + (id >> 2u);
    return id == 0 ? 1 : id;
}

bool reads(RenderImageAccess access) {
    switch (access) {
    case RenderImageAccess::ColorAttachmentReadWrite:
    case RenderImageAccess::DepthAttachmentRead:
    case RenderImageAccess::SampledRead:
    case RenderImageAccess::StorageReadWrite:
    case RenderImageAccess::TransferRead:
        return true;
    default:
        return false;
    }
}

bool writes(RenderImageAccess access) {
    switch (access) {
    case RenderImageAccess::ColorAttachmentWrite:
    case RenderImageAccess::ColorAttachmentReadWrite:
    case RenderImageAccess::DepthAttachmentWrite:
    case RenderImageAccess::StorageWrite:
    case RenderImageAccess::StorageReadWrite:
    case RenderImageAccess::TransferWrite:
        return true;
    default:
        return false;
    }
}

bool reads(RgBufferAccess access) {
    return access != RgBufferAccess::StorageWrite &&
           access != RgBufferAccess::TransferWrite &&
           access != RgBufferAccess::AccelerationStructureBuildWrite;
}

bool writes(RgBufferAccess access) {
    return access == RgBufferAccess::StorageWrite ||
           access == RgBufferAccess::StorageReadWrite ||
           access == RgBufferAccess::TransferWrite ||
           access == RgBufferAccess::AccelerationStructureBuildWrite;
}

RenderGraphBufferState stateFor(RgBufferAccess access) {
    RenderGraphBufferState state{};
    state.initialized = true;
    switch (access) {
    case RgBufferAccess::UniformRead:
        state.stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        state.access = VK_ACCESS_2_UNIFORM_READ_BIT;
        break;
    case RgBufferAccess::StorageRead:
        state.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        break;
    case RgBufferAccess::StorageWrite:
        state.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        break;
    case RgBufferAccess::StorageReadWrite:
        state.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        break;
    case RgBufferAccess::VertexRead:
        state.stage = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        state.access = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        break;
    case RgBufferAccess::IndexRead:
        state.stage = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        state.access = VK_ACCESS_2_INDEX_READ_BIT;
        break;
    case RgBufferAccess::IndirectRead:
        state.stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        state.access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        break;
    case RgBufferAccess::TransferRead:
        state.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        state.access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case RgBufferAccess::TransferWrite:
        state.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        state.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case RgBufferAccess::AccelerationStructureBuildRead:
        state.stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        state.access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        break;
    case RgBufferAccess::AccelerationStructureBuildWrite:
        state.stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        state.access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        break;
    case RgBufferAccess::AccelerationStructureRead:
        state.stage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        state.access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        break;
    }
    return state;
}

RenderGraphImageState stateFor(RenderImageAccess access,
                               VkImageLayout layout) {
    RenderGraphImageState state{};
    state.layout = layout;
    state.initialized = layout != VK_IMAGE_LAYOUT_UNDEFINED;
    switch (access) {
    case RenderImageAccess::ColorAttachmentWrite:
        state.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        state.access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case RenderImageAccess::ColorAttachmentReadWrite:
        state.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        state.access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case RenderImageAccess::DepthAttachmentWrite:
        state.stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        state.access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case RenderImageAccess::DepthAttachmentRead:
        state.stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        state.access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case RenderImageAccess::SampledRead:
        state.stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case RenderImageAccess::StorageWrite:
        state.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        break;
    case RenderImageAccess::StorageReadWrite:
        state.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        state.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        break;
    case RenderImageAccess::TransferRead:
        state.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        state.access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case RenderImageAccess::TransferWrite:
        state.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        state.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    }
    return state;
}

bool hasWriteAccess(VkAccessFlags2 access) {
    constexpr VkAccessFlags2 writesMask =
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    return (access & writesMask) != 0;
}

const Image &usageImage(const RenderResourceRegistry &resources,
                        const RenderImageUsage &usage,
                        uint32_t frameIndex) {
    return usage.frame == RenderImageFrame::Previous
               ? resources.previousImage(usage.image, frameIndex)
               : resources.image(usage.image, frameIndex);
}

uint64_t imageStateKey(VkImage image, uint32_t mip, uint32_t layer,
                       VkImageAspectFlags aspect) {
    uint64_t key = reinterpret_cast<uint64_t>(image);
    const auto mix = [&key](uint64_t value) {
        key ^= value + 0x9e3779b97f4a7c15ull + (key << 6u) + (key >> 2u);
    };
    mix(mip);
    mix(layer);
    mix(aspect);
    return key;
}

void transitionUsage(
    VkCommandBuffer cmd, const RenderResourceRegistry &resources,
    const RenderImageUsage &usage, const RgImageSubresource &subresource,
    uint32_t frameIndex,
    VkImageLayout targetLayout, RenderGraphImageState targetState,
    std::unordered_map<uint64_t, RenderGraphImageState> &states,
    RenderGraphDiagnostics *diagnostics) {
    const Image &image = usageImage(resources, usage, frameIndex);
    const RenderImageDesc &desc = resources.description(usage.image);
    targetState.layout = targetLayout;
    targetState.initialized = targetLayout != VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(subresource.levelCount * subresource.layerCount);
    for (uint32_t mip = subresource.baseMipLevel;
         mip < subresource.baseMipLevel + subresource.levelCount; ++mip) {
        for (uint32_t layer = subresource.baseArrayLayer;
             layer < subresource.baseArrayLayer + subresource.layerCount;
             ++layer) {
            const uint64_t key = imageStateKey(
                image.handle(), mip, layer, subresource.aspectMask);
            RenderGraphImageState &current = states[key];
            if (!current.initialized && desc.externallyInitialized) {
                current.layout = desc.initialLayout;
                current.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                current.access = VK_ACCESS_2_MEMORY_READ_BIT |
                                 VK_ACCESS_2_MEMORY_WRITE_BIT;
                current.initialized = true;
            }
            const bool layoutChange = current.layout != targetLayout;
            const bool hazard = hasWriteAccess(current.access) ||
                                hasWriteAccess(targetState.access);
            if (targetLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
                (layoutChange || hazard)) {
                VkImageMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = current.initialized
                                           ? current.stage
                                           : VK_PIPELINE_STAGE_2_NONE;
                barrier.srcAccessMask = current.initialized
                                            ? current.access
                                            : VK_ACCESS_2_NONE;
                barrier.dstStageMask = targetState.stage;
                barrier.dstAccessMask = targetState.access;
                barrier.oldLayout = current.initialized
                                        ? current.layout
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = targetLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.handle();
                barrier.subresourceRange = {
                    subresource.aspectMask, mip, 1, layer, 1};
                barriers.push_back(barrier);
                if (diagnostics) {
                    ++diagnostics->automaticBarriers;
                    if (layoutChange)
                        ++diagnostics->layoutBarriers;
                    if (hazard)
                        ++diagnostics->hazardBarriers;
                }
            }
            current = targetState;
        }
    }
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount =
            static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
}

void transitionImportedImage(
    VkCommandBuffer cmd, VkImage image, RenderImageAccess access,
    const RgImageSubresource &subresource, VkImageLayout targetLayout,
    std::unordered_map<uint64_t, RenderGraphImageState> &states,
    RenderGraphDiagnostics *diagnostics) {
    if (image == VK_NULL_HANDLE || targetLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        return;
    RenderGraphImageState targetState = stateFor(access, targetLayout);
    if (targetLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        targetState.stage = VK_PIPELINE_STAGE_2_NONE;
        targetState.access = VK_ACCESS_2_NONE;
    }
    std::vector<VkImageMemoryBarrier2> barriers;
    for (uint32_t mip = subresource.baseMipLevel;
         mip < subresource.baseMipLevel + subresource.levelCount; ++mip) {
        for (uint32_t layer = subresource.baseArrayLayer;
             layer < subresource.baseArrayLayer + subresource.layerCount;
             ++layer) {
            RenderGraphImageState &current = states[imageStateKey(
                image, mip, layer, subresource.aspectMask)];
            const bool layoutChange = current.layout != targetLayout;
            const bool hazard = hasWriteAccess(current.access) ||
                                hasWriteAccess(targetState.access);
            if (layoutChange || hazard) {
                VkImageMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = current.initialized
                                           ? current.stage
                                           : VK_PIPELINE_STAGE_2_NONE;
                barrier.srcAccessMask = current.initialized
                                            ? current.access
                                            : VK_ACCESS_2_NONE;
                barrier.dstStageMask = targetState.stage;
                barrier.dstAccessMask = targetState.access;
                barrier.oldLayout = current.initialized
                                        ? current.layout
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = targetLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {
                    subresource.aspectMask, mip, 1, layer, 1};
                barriers.push_back(barrier);
                if (diagnostics) {
                    ++diagnostics->automaticBarriers;
                    if (layoutChange)
                        ++diagnostics->layoutBarriers;
                    if (hazard)
                        ++diagnostics->hazardBarriers;
                }
            }
            current = targetState;
            current.initialized = true;
        }
    }
    if (!barriers.empty()) {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount =
            static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
}

void transitionBuffer(
    VkCommandBuffer cmd, const RenderGraphBufferUse &use,
    std::unordered_map<uint64_t, RenderGraphBufferState> &states,
    RenderGraphDiagnostics *diagnostics) {
    if (use.buffer == VK_NULL_HANDLE)
        return;
    const uint64_t key = reinterpret_cast<uint64_t>(use.buffer);
    RenderGraphBufferState &current = states[key];
    const RenderGraphBufferState target = stateFor(use.access);
    const bool hazard = current.initialized &&
                        (hasWriteAccess(current.access) ||
                         hasWriteAccess(target.access));
    if (hazard) {
        VkBufferMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = current.stage;
        barrier.srcAccessMask = current.access;
        barrier.dstStageMask = target.stage;
        barrier.dstAccessMask = target.access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = use.buffer;
        barrier.offset = use.offset;
        barrier.size = use.size;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (diagnostics) {
            ++diagnostics->automaticBarriers;
            ++diagnostics->hazardBarriers;
        }
    }
    current = target;
}

uint64_t logicalImageKey(const RenderImageUsage &usage) {
    return (static_cast<uint64_t>(usage.image.index) << 1u) |
           (usage.frame == RenderImageFrame::Previous ? 1u : 0u);
}

uint64_t imageAccessKey(const RenderImageUsage &usage, uint32_t mip,
                        uint32_t layer, VkImageAspectFlags aspect) {
    uint64_t key = logicalImageKey(usage);
    const auto mix = [&key](uint64_t value) {
        key ^= value + 0x9e3779b97f4a7c15ull + (key << 6u) +
               (key >> 2u);
    };
    mix(mip);
    mix(layer);
    mix(aspect);
    return key;
}

void hashCombine(uint64_t &seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

bool conditionActive(RgPassCondition condition,
                     const FrameRenderFeatures &features) {
    switch (condition) {
    case RgPassCondition::Always:
        return true;
    case RgPassCondition::Atmosphere:
        return features.atmosphereRequired;
    case RgPassCondition::DirectionalShadow:
        return features.directionalShadowRequired;
    case RgPassCondition::PointShadow:
        return features.pointShadowRequired;
    case RgPassCondition::SpotShadow:
        return features.spotShadowRequired;
    case RgPassCondition::SurfaceData:
        return features.surfaceDataRequired;
    case RgPassCondition::HiZ:
        return features.hiZRequired;
    case RgPassCondition::Occlusion:
        return features.occlusionRequired;
    case RgPassCondition::ScreenDepthPyramid:
        return features.screenDepthPyramidRequired;
    case RgPassCondition::SceneColorPyramid:
        return features.sceneColorPyramidRequired;
    case RgPassCondition::Ssao:
        return features.ssaoRequired;
    case RgPassCondition::Cacao:
        return features.cacaoRequired;
    case RgPassCondition::Gtao:
        return features.gtaoRequired;
    case RgPassCondition::Ddgi:
        return features.ddgiRequired;
    case RgPassCondition::Ssr:
        return features.ssrRequired;
    case RgPassCondition::Ssgi:
        return features.ssgiRequired;
    case RgPassCondition::Taa:
        return features.taaRequired;
    case RgPassCondition::Bloom:
        return features.bloomRequired;
    case RgPassCondition::Capture:
        return features.captureRequired;
    }
    return false;
}

uint64_t featureKey(
    const std::vector<std::unique_ptr<IRenderPass>> &passes,
    const RenderResourceRegistry &resources,
    const FrameRenderFeatures &features) {
    uint64_t key = 0;
    for (uint32_t index = 0; index < passes.size(); ++index) {
        hashCombine(key, index);
        hashCombine(key, conditionActive(passes[index]->condition(), features));
        hashCombine(key, passes[index]->topologySignature());
    }
    hashCombine(key, features.directionalShadowCascadeCount);
    hashCombine(key, features.pointShadowLightCount);
    hashCombine(key, features.spotShadowLightCount);
    hashCombine(key, features.atmosphereRequired);
    hashCombine(key, features.directionalShadowRequired);
    hashCombine(key, features.pointShadowRequired);
    hashCombine(key, features.spotShadowRequired);
    hashCombine(key, features.surfaceDataRequired);
    hashCombine(key, features.hiZRequired);
    hashCombine(key, features.occlusionRequired);
    hashCombine(key, features.screenDepthPyramidRequired);
    hashCombine(key, features.sceneColorPyramidRequired);
    hashCombine(key, features.ssaoRequired);
    hashCombine(key, features.ssaoActive);
    hashCombine(key, features.cacaoRequired);
    hashCombine(key, features.cacaoActive);
    hashCombine(key, features.gtaoRequired);
    hashCombine(key, features.gtaoActive);
    hashCombine(key, features.taaRequired);
    hashCombine(key, features.taaActive);
    hashCombine(key, features.ssrRequired);
    hashCombine(key, features.ssrActive);
    hashCombine(key, features.ssgiRequired);
    hashCombine(key, features.ssgiActive);
    hashCombine(key, features.ddgiRequired);
    hashCombine(key, features.ddgiActive);
    hashCombine(key, features.bloomRequired);
    hashCombine(key, features.captureRequired);
    hashCombine(key, features.captureSource
                         ? static_cast<uint64_t>(*features.captureSource) + 1u
                         : 0u);
    for (const RenderImageDesc &desc : resources.imageDescriptions()) {
        hashCombine(key, desc.format);
        hashCombine(key, desc.samples);
        hashCombine(key, desc.arrayLayers);
        hashCombine(key, desc.mipLevels);
    }
    return key;
}

RenderGraphDiagnostics makeDiagnostics(
    const CompiledRenderGraph &compiled,
    const RenderResourceRegistry &resources) {
    RenderGraphDiagnostics diagnostics{};
    diagnostics.topologyHash = compiled.topologyHash;
    diagnostics.activePasses =
        static_cast<uint32_t>(compiled.executionOrder.size());
    diagnostics.culledPasses =
        static_cast<uint32_t>(compiled.culledPasses.size());
    diagnostics.dependencyEdges =
        static_cast<uint32_t>(compiled.dependencies.size());
    diagnostics.residentImageBytes = resources.estimatedResidentBytes();
    std::unordered_set<uint32_t> activeImages;
    std::unordered_map<uint32_t, uint32_t> resourceRows;
    std::unordered_map<uint32_t, uint32_t> bufferRows;
    for (uint32_t passIndex : compiled.executionOrder) {
        const auto &pass = compiled.passes[passIndex];
        diagnostics.executionOrder.push_back(pass.name);
        for (const auto &image : pass.images) {
            activeImages.insert(image.physical.image.index);
            const uint32_t imageIndex = image.physical.image.index;
            auto [rowIt, inserted] = resourceRows.try_emplace(
                imageIndex,
                static_cast<uint32_t>(diagnostics.resources.size()));
            if (inserted) {
                const RenderImageDesc &desc =
                    resources.description({imageIndex});
                RenderGraphDiagnostics::Resource row{};
                row.index = imageIndex;
                row.name = desc.name;
                row.lifetime =
                    image.lifetime == RgResourceLifetime::History
                        ? "History"
                        : image.lifetime == RgResourceLifetime::PerFrame
                              ? "PerFrame"
                              : image.lifetime ==
                                        RgResourceLifetime::Persistent
                                    ? "Persistent"
                                    : image.lifetime ==
                                              RgResourceLifetime::Imported
                                          ? "Imported"
                                          : "Transient";
                row.residentBytes = resources.estimatedBytes({imageIndex});
                row.initialLayout = desc.initialLayout;
                diagnostics.resources.push_back(std::move(row));
            }
            auto &row = diagnostics.resources[rowIt->second];
            row.versions = std::max(row.versions, image.after.version);
            row.finalLayout = image.physical.finalLayout !=
                                      VK_IMAGE_LAYOUT_UNDEFINED
                                  ? image.physical.finalLayout
                                  : image.physical.requiredLayout;
            auto &names = writes(image.physical.access) ? row.producers
                                                        : row.consumers;
            if (std::find(names.begin(), names.end(), pass.name) ==
                names.end()) {
                names.push_back(pass.name);
            }
            if (reads(image.physical.access) &&
                writes(image.physical.access) &&
                std::find(row.consumers.begin(), row.consumers.end(),
                          pass.name) == row.consumers.end()) {
                row.consumers.push_back(pass.name);
            }
        }
        for (const RenderGraphBufferUse &buffer : pass.buffers) {
            auto [rowIt, inserted] = bufferRows.try_emplace(
                buffer.before.resource,
                static_cast<uint32_t>(diagnostics.buffers.size()));
            if (inserted) {
                RenderGraphDiagnostics::BufferResource row{};
                row.index = buffer.before.resource;
                row.nativeHandle =
                    reinterpret_cast<uint64_t>(buffer.buffer);
                std::ostringstream name;
                name << "Buffer 0x" << std::hex << row.nativeHandle;
                row.name = name.str();
                row.lifetime = buffer.frameSlot == UINT32_MAX
                                   ? "Persistent"
                                   : "PerFrame";
                diagnostics.buffers.push_back(std::move(row));
            }
            auto &row = diagnostics.buffers[rowIt->second];
            row.versions = std::max(row.versions, buffer.after.version);
            if (buffer.size != VK_WHOLE_SIZE)
                row.declaredRangeBytes =
                    std::max(row.declaredRangeBytes,
                             static_cast<uint64_t>(buffer.offset +
                                                   buffer.size));
            auto &names = writes(buffer.access) ? row.producers
                                                : row.consumers;
            if (std::find(names.begin(), names.end(), pass.name) ==
                names.end()) {
                names.push_back(pass.name);
            }
            if (reads(buffer.access) && writes(buffer.access) &&
                std::find(row.consumers.begin(), row.consumers.end(),
                          pass.name) == row.consumers.end()) {
                row.consumers.push_back(pass.name);
            }
        }
    }
    for (uint32_t passIndex : compiled.culledPasses)
        diagnostics.culledNames.push_back(compiled.passes[passIndex].name);
    for (uint32_t image : activeImages) {
        if (image != kInvalidRenderResource)
            diagnostics.activeImageBytes += resources.estimatedBytes({image});
    }
    std::sort(diagnostics.resources.begin(), diagnostics.resources.end(),
              [](const auto &left, const auto &right) {
                  return left.index < right.index;
              });
    std::sort(diagnostics.buffers.begin(), diagnostics.buffers.end(),
              [](const auto &left, const auto &right) {
                  return left.index < right.index;
              });
    return diagnostics;
}

} // namespace

RenderGraphBuilder::RenderGraphBuilder(
    const RenderResourceRegistry &resources,
    const FrameRenderFeatures &features)
    : resources_(&resources), features_(&features) {}

void RenderGraphBuilder::beginPass(uint32_t registrationIndex,
                                   const IRenderPass &pass) {
    pendingPass_ = &pass;
    pendingPassIndex_ = registrationIndex;
    currentPass_ = nullptr;
}

void RenderGraphBuilder::addNode(std::string name, RgPassType type,
                                 RgQueueClass queue,
                                 uint32_t localNodeIndex) {
    if (!pendingPass_)
        throw std::logic_error("RenderGraphBuilder node has no owner pass");
    RenderGraphPassDeclaration declaration{};
    declaration.registrationIndex = static_cast<uint32_t>(passes_.size());
    declaration.ownerPassIndex = pendingPassIndex_;
    declaration.localNodeIndex = localNodeIndex;
    declaration.groupName = pendingPass_->name();
    declaration.groupId =
        groupPassId(declaration.groupName, pendingPassIndex_);
    declaration.name = std::move(name);
    declaration.type = type;
    declaration.queue = queue;
    declaration.active =
        conditionActive(pendingPass_->condition(), *features_);
    uint64_t id = 1469598103934665603ull;
    for (const unsigned char c : declaration.name) {
        id ^= c;
        id *= 1099511628211ull;
    }
    hashCombine(id, pendingPassIndex_);
    hashCombine(id, localNodeIndex);
    declaration.id = id;
    passes_.push_back(std::move(declaration));
    currentPass_ = &passes_.back();
}

void RenderGraphBuilder::useImage(RenderImageUsage usage,
                                  RgImageSubresource subresource) {
    if (!currentPass_)
        throw std::logic_error("RenderGraphBuilder image use has no pass");
    if (!resources_->valid(usage.image))
        throw std::invalid_argument("RenderGraphBuilder received an invalid image");
    const RenderImageDesc &desc = resources_->description(usage.image);
    const uint32_t logicalResource =
        usage.image.index * 2u +
        (usage.frame == RenderImageFrame::Previous ? 1u : 0u);
    uint32_t &version = imageVersions_[logicalResource];
    const RgImageHandle before{logicalResource, version};
    if (writes(usage.access))
        ++version;
    const RgImageHandle after{logicalResource, version};
    if (subresource.aspectMask == 0)
        subresource.aspectMask = desc.aspect;
    if (subresource.levelCount == VK_REMAINING_MIP_LEVELS)
        subresource.levelCount = resources_->mipLevelCount(usage.image) -
                                 subresource.baseMipLevel;
    if (subresource.layerCount == VK_REMAINING_ARRAY_LAYERS)
        subresource.layerCount = desc.arrayLayers -
                                 subresource.baseArrayLayer;
    RgResourceLifetime lifetime = RgResourceLifetime::Transient;
    if (desc.externallyInitialized)
        lifetime = RgResourceLifetime::Imported;
    else if (desc.historyCapable)
        lifetime = RgResourceLifetime::History;
    else if (desc.multiplicity == RenderResourceMultiplicity::PerFrame)
        lifetime = RgResourceLifetime::PerFrame;
    else
        lifetime = RgResourceLifetime::Persistent;
    currentPass_->images.push_back(
        {before, after, usage, subresource, lifetime});
}

void RenderGraphBuilder::useBuffer(VkBuffer buffer, RgBufferAccess access,
                                   VkDeviceSize offset, VkDeviceSize size,
                                   uint32_t frameSlot) {
    if (!currentPass_)
        throw std::logic_error("RenderGraphBuilder buffer use has no pass");
    if (buffer == VK_NULL_HANDLE)
        throw std::invalid_argument("RenderGraphBuilder received a null buffer");
    const uint64_t key = reinterpret_cast<uint64_t>(buffer);
    auto [resourceIt, inserted] =
        bufferResources_.try_emplace(key, nextBufferResource_);
    if (inserted)
        ++nextBufferResource_;
    uint32_t &version = bufferVersions_[key];
    const RgBufferHandle before{resourceIt->second, version};
    const bool write = access == RgBufferAccess::StorageWrite ||
                       access == RgBufferAccess::StorageReadWrite ||
                       access == RgBufferAccess::TransferWrite ||
                       access == RgBufferAccess::AccelerationStructureBuildWrite;
    if (write)
        ++version;
    currentPass_->buffers.push_back(
        {before, {resourceIt->second, version}, buffer, offset, size, access,
         frameSlot});
}

void RenderGraphBuilder::useSwapchainImage(
    RenderImageAccess access, VkImageLayout requiredLayout,
    VkImageLayout finalLayout, RgImageSubresource subresource) {
    if (!currentPass_)
        throw std::logic_error(
            "RenderGraphBuilder imported image use has no pass");
    currentPass_->importedImages.push_back(
        {RgImportedImageKind::Swapchain, access, requiredLayout, finalLayout,
         subresource});
}

void RenderGraphBuilder::addColorAttachment(
    RenderImageHandle image, RenderImageAccess access,
    VkImageLayout finalLayout, VkAttachmentLoadOp loadOp,
    VkAttachmentStoreOp storeOp, VkClearColorValue clearValue,
    RgImageSubresource subresource, RenderImageHandle resolveImage,
    VkImageLayout resolveFinalLayout,
    VkResolveModeFlagBits resolveMode) {
    const RenderImageDesc &desc = resources_->description(image);
    if (subresource.aspectMask == 0)
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (subresource.levelCount == VK_REMAINING_MIP_LEVELS)
        subresource.levelCount = 1;
    if (subresource.layerCount == VK_REMAINING_ARRAY_LAYERS)
        subresource.layerCount = desc.arrayLayers == 1 ? 1 : desc.arrayLayers;
    const VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    useImage({image, access, layout, finalLayout}, subresource);
    RenderGraphAttachment attachment{};
    attachment.kind = RgAttachmentKind::Color;
    attachment.image = image;
    attachment.subresource = subresource;
    attachment.layout = layout;
    attachment.loadOp = loadOp;
    attachment.storeOp = storeOp;
    attachment.clearValue.color = clearValue;
    if (resolveImage.valid()) {
        if (resolveFinalLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            resolveFinalLayout = layout;
        useImage({resolveImage, RenderImageAccess::ColorAttachmentWrite,
                  layout, resolveFinalLayout}, subresource);
        attachment.resolveImage = resolveImage;
        attachment.resolveLayout = layout;
        attachment.resolveMode = resolveMode;
    }
    currentPass_->attachments.push_back(attachment);
}

void RenderGraphBuilder::addDepthAttachment(
    RenderImageHandle image, RenderImageAccess access,
    VkImageLayout requiredLayout, VkImageLayout finalLayout,
    VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
    VkClearDepthStencilValue clearValue,
    RgImageSubresource subresource) {
    const RenderImageDesc &desc = resources_->description(image);
    if (subresource.aspectMask == 0)
        subresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (subresource.levelCount == VK_REMAINING_MIP_LEVELS)
        subresource.levelCount = 1;
    if (subresource.layerCount == VK_REMAINING_ARRAY_LAYERS)
        subresource.layerCount = desc.arrayLayers == 1 ? 1 : desc.arrayLayers;
    useImage({image, access, requiredLayout, finalLayout}, subresource);
    RenderGraphAttachment attachment{};
    attachment.kind = RgAttachmentKind::Depth;
    attachment.image = image;
    attachment.subresource = subresource;
    attachment.layout = requiredLayout;
    attachment.loadOp = loadOp;
    attachment.storeOp = storeOp;
    attachment.clearValue.depthStencil = clearValue;
    currentPass_->attachments.push_back(attachment);
}

void RenderGraphBuilder::addSwapchainColorAttachment(
    VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
    VkClearColorValue clearValue) {
    useSwapchainImage(RenderImageAccess::ColorAttachmentWrite,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    RenderGraphAttachment attachment{};
    attachment.kind = RgAttachmentKind::Color;
    attachment.importedSwapchain = true;
    attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = loadOp;
    attachment.storeOp = storeOp;
    attachment.clearValue.color = clearValue;
    attachment.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    currentPass_->attachments.push_back(attachment);
}

void RenderGraphBuilder::dependsOn(RenderGraphPassId pass) {
    if (!currentPass_)
        throw std::logic_error("RenderGraphBuilder dependency has no pass");
    currentPass_->explicitDependencies.push_back(pass);
}

void RenderGraphBuilder::setSideEffect(bool enabled) {
    if (!currentPass_)
        throw std::logic_error("RenderGraphBuilder side effect has no pass");
    currentPass_->sideEffect = enabled;
}

void RenderGraphBuilder::setActive(bool active) {
    if (!currentPass_)
        throw std::logic_error("RenderGraphBuilder active state has no pass");
    currentPass_->active = currentPass_->active && active;
}

CompiledRenderGraph RenderGraphCompiler::compile(
    const std::vector<std::unique_ptr<IRenderPass>> &passes,
    const RenderResourceRegistry &resources,
    const FrameRenderFeatures &features) {
    RenderGraphBuilder builder(resources, features);
    const RenderGraphBuildContext buildContext{features, resources};
    for (uint32_t index = 0; index < passes.size(); ++index) {
        builder.beginPass(index, *passes[index]);
        passes[index]->setup(builder, buildContext);
    }
    std::vector<RenderGraphPassDeclaration> declarations = builder.passes();
    uint32_t presentWriters = 0;
    for (const auto &pass : declarations) {
        if (!pass.active)
            continue;
        for (const auto &image : pass.importedImages) {
            if (image.kind == RgImportedImageKind::Swapchain &&
                writes(image.access) &&
                image.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                ++presentWriters;
            }
        }
    }
    if (presentWriters != 1) {
        throw std::runtime_error(
            "RenderGraph requires exactly one active swapchain Present writer");
    }
    for (const auto &pass : declarations) {
        if (!pass.active || pass.attachments.empty())
            continue;
        if (pass.type != RgPassType::Graphics) {
            throw std::runtime_error(pass.name +
                                     " declares attachments on a non-graphics node");
        }

        std::optional<VkExtent2D> attachmentExtent;
        std::optional<VkSampleCountFlagBits> attachmentSamples;
        std::optional<uint32_t> attachmentLayers;
        bool hasDepth = false;
        bool hasStencil = false;
        for (const RenderGraphAttachment &attachment : pass.attachments) {
            if (attachment.importedSwapchain) {
                if (attachment.kind != RgAttachmentKind::Color) {
                    throw std::runtime_error(
                        pass.name + " declares the swapchain as a non-color attachment");
                }
                continue;
            }
            if (!attachment.image.valid()) {
                throw std::runtime_error(pass.name +
                                         " declares an invalid attachment image");
            }
            const RenderImageDesc &desc =
                resources.description(attachment.image);
            if (attachment.subresource.levelCount != 1 ||
                attachment.subresource.layerCount == 0) {
                throw std::runtime_error(
                    pass.name + " attachment views must select one mip and at least one layer");
            }
            if (attachmentLayers &&
                *attachmentLayers != attachment.subresource.layerCount) {
                throw std::runtime_error(pass.name +
                                         " has attachments with mismatched layer counts");
            }
            attachmentLayers = attachment.subresource.layerCount;
            const VkExtent2D baseExtent = resources.extent(attachment.image);
            const VkExtent2D extent{
                std::max(1u, baseExtent.width >>
                                 attachment.subresource.baseMipLevel),
                std::max(1u, baseExtent.height >>
                                 attachment.subresource.baseMipLevel)};
            if (attachmentExtent &&
                (attachmentExtent->width != extent.width ||
                 attachmentExtent->height != extent.height)) {
                throw std::runtime_error(pass.name +
                                         " has attachments with mismatched extents");
            }
            attachmentExtent = extent;
            if (attachmentSamples && *attachmentSamples != desc.samples) {
                throw std::runtime_error(pass.name +
                                         " has attachments with mismatched sample counts");
            }
            attachmentSamples = desc.samples;

            const RenderGraphImageUse *declaredUse = nullptr;
            for (const RenderGraphImageUse &use : pass.images) {
                if (use.physical.image == attachment.image &&
                    use.subresource.baseMipLevel ==
                        attachment.subresource.baseMipLevel &&
                    use.subresource.baseArrayLayer ==
                        attachment.subresource.baseArrayLayer) {
                    declaredUse = &use;
                    break;
                }
            }
            if (!declaredUse) {
                throw std::runtime_error(pass.name +
                                         " attachment is missing its image usage declaration");
            }
            if (attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                !reads(declaredUse->physical.access)) {
                throw std::runtime_error(pass.name +
                                         " uses LOAD with a write-only attachment access");
            }

            if (attachment.kind == RgAttachmentKind::Color) {
                if ((desc.aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0 ||
                    (attachment.subresource.aspectMask &
                     VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
                    throw std::runtime_error(pass.name +
                                             " declares a non-color image as a color attachment");
                }
            } else if (attachment.kind == RgAttachmentKind::Depth) {
                if (hasDepth || (desc.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
                    throw std::runtime_error(pass.name +
                                             " has an invalid depth attachment declaration");
                }
                hasDepth = true;
            } else {
                if (hasStencil ||
                    (desc.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) == 0) {
                    throw std::runtime_error(pass.name +
                                             " has an invalid stencil attachment declaration");
                }
                hasStencil = true;
            }

            if (attachment.resolveImage.valid()) {
                const RenderImageDesc &resolveDesc =
                    resources.description(attachment.resolveImage);
                const VkExtent2D resolveBase =
                    resources.extent(attachment.resolveImage);
                const VkExtent2D resolveExtent{
                    std::max(1u, resolveBase.width >>
                                     attachment.subresource.baseMipLevel),
                    std::max(1u, resolveBase.height >>
                                     attachment.subresource.baseMipLevel)};
                if (attachment.kind != RgAttachmentKind::Color ||
                    desc.samples == VK_SAMPLE_COUNT_1_BIT ||
                    resolveDesc.samples != VK_SAMPLE_COUNT_1_BIT ||
                    resolveDesc.format != desc.format ||
                    resolveExtent.width != extent.width ||
                    resolveExtent.height != extent.height ||
                    attachment.resolveMode == VK_RESOLVE_MODE_NONE) {
                    throw std::runtime_error(pass.name +
                                             " has an incompatible resolve attachment");
                }
            }
        }
    }
    std::vector<RenderPassResourceUsage> contracts;
    contracts.reserve(declarations.size());
    for (const auto &pass : declarations) {
        if (!pass.active)
            continue;
        RenderPassResourceUsage contract{};
        contract.passName = pass.name;
        for (const RenderGraphImageUse &image : pass.images)
            contract.images.push_back(image.physical);
        contracts.push_back(std::move(contract));
    }

    validateRenderResourceContracts(resources.imageDescriptions(), contracts,
                                    false);

    struct AccessHistory {
        int32_t writer = -1;
        std::vector<uint32_t> readers;
    };
    std::unordered_map<uint64_t, AccessHistory> history;
    std::vector<std::unordered_set<uint32_t>> dependencySets(
        declarations.size());
    std::vector<RenderGraphDependency> dependencies;

    const auto addDependency = [&](uint32_t producer, uint32_t consumer,
                                   uint32_t resource) {
        if (producer == consumer ||
            !dependencySets[consumer].insert(producer).second)
            return;
        dependencies.push_back({producer, consumer, resource});
    };

    std::unordered_set<uint64_t> externallyInitializedImages;
    for (uint32_t imageIndex = 0;
         imageIndex < resources.imageDescriptions().size(); ++imageIndex) {
        if (!resources.imageDescriptions()[imageIndex].externallyInitialized)
            continue;
        const RenderImageHandle handle{imageIndex};
        const RenderImageDesc &desc = resources.description(handle);
        for (uint32_t frameKind = 0; frameKind < 2; ++frameKind) {
            RenderImageUsage usage{};
            usage.image = handle;
            usage.frame = frameKind == 0 ? RenderImageFrame::Current
                                         : RenderImageFrame::Previous;
            for (uint32_t mip = 0; mip < resources.mipLevelCount(handle);
                 ++mip) {
                for (uint32_t layer = 0; layer < desc.arrayLayers; ++layer) {
                    externallyInitializedImages.insert(imageAccessKey(
                        usage, mip, layer, desc.aspect));
                }
            }
        }
    }

    for (uint32_t passIndex = 0; passIndex < declarations.size(); ++passIndex) {
        if (!declarations[passIndex].active)
            continue;
        for (const RenderGraphImageUse &image : declarations[passIndex].images) {
            const RenderImageUsage &usage = image.physical;
            for (uint32_t mip = image.subresource.baseMipLevel;
                 mip < image.subresource.baseMipLevel +
                           image.subresource.levelCount;
                 ++mip) {
                for (uint32_t layer = image.subresource.baseArrayLayer;
                     layer < image.subresource.baseArrayLayer +
                                 image.subresource.layerCount;
                     ++layer) {
                    const uint64_t accessKey = imageAccessKey(
                        usage, mip, layer, image.subresource.aspectMask);
                    AccessHistory &state = history[accessKey];
                    if (reads(usage.access) && state.writer < 0 &&
                        usage.frame != RenderImageFrame::Previous &&
                        externallyInitializedImages.count(accessKey) == 0) {
                        throw std::runtime_error(
                            declarations[passIndex].name + " reads " +
                            resources.description(usage.image).name +
                            " before a writer");
                    }
                    if (reads(usage.access) && state.writer >= 0)
                        addDependency(
                            static_cast<uint32_t>(state.writer), passIndex,
                            usage.image.index);
                    if (writes(usage.access)) {
                        if (state.writer >= 0)
                            addDependency(
                                static_cast<uint32_t>(state.writer),
                                passIndex, usage.image.index);
                        for (uint32_t reader : state.readers)
                            addDependency(reader, passIndex,
                                          usage.image.index);
                        state.readers.clear();
                        state.writer = static_cast<int32_t>(passIndex);
                    } else if (reads(usage.access)) {
                        state.readers.push_back(passIndex);
                    }
                }
            }
        }
        for (const RenderGraphBufferUse &buffer :
             declarations[passIndex].buffers) {
            AccessHistory &state = history[
                (1ull << 63u) | buffer.before.resource];
            if (reads(buffer.access) && state.writer >= 0) {
                addDependency(static_cast<uint32_t>(state.writer), passIndex,
                              kInvalidRenderResource);
            }
            if (writes(buffer.access)) {
                if (state.writer >= 0) {
                    addDependency(static_cast<uint32_t>(state.writer),
                                  passIndex, kInvalidRenderResource);
                }
                for (uint32_t reader : state.readers)
                    addDependency(reader, passIndex, kInvalidRenderResource);
                state.readers.clear();
                state.writer = static_cast<int32_t>(passIndex);
            } else if (reads(buffer.access)) {
                state.readers.push_back(passIndex);
            }
        }
    }

    std::unordered_map<RenderGraphPassId, uint32_t> passById;
    for (const auto &pass : declarations)
        passById.emplace(pass.id, pass.registrationIndex);
    for (const auto &pass : declarations) {
        if (!pass.active)
            continue;
        for (RenderGraphPassId dependency : pass.explicitDependencies) {
            const auto it = passById.find(dependency);
            if (it == passById.end())
                throw std::runtime_error("RenderGraph explicit dependency references an unknown pass");
            if (!declarations[it->second].active)
                throw std::runtime_error(
                    "RenderGraph active pass depends on an inactive pass");
            addDependency(it->second, pass.registrationIndex,
                          kInvalidRenderResource);
        }
    }

    // External and transfer side effects are sequence boundaries. This keeps
    // AS builds before their consumers and screenshot copies after Present,
    // even when they do not expose a graph-owned resource.
    for (uint32_t index = 0; index < declarations.size(); ++index) {
        if (!declarations[index].active || !declarations[index].sideEffect)
            continue;
        for (uint32_t previous = index; previous-- > 0;) {
            if (declarations[previous].active) {
                addDependency(previous, index, kInvalidRenderResource);
                break;
            }
        }
        for (uint32_t next = index + 1; next < declarations.size(); ++next) {
            if (declarations[next].active) {
                addDependency(index, next, kInvalidRenderResource);
                break;
            }
        }
    }

    std::vector<std::vector<uint32_t>> outgoing(declarations.size());
    std::vector<uint32_t> indegree(declarations.size(), 0);
    for (uint32_t consumer = 0; consumer < dependencySets.size(); ++consumer) {
        indegree[consumer] =
            static_cast<uint32_t>(dependencySets[consumer].size());
        for (uint32_t producer : dependencySets[consumer])
            outgoing[producer].push_back(consumer);
    }

    std::priority_queue<uint32_t, std::vector<uint32_t>,
                        std::greater<uint32_t>> ready;
    for (uint32_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0)
            ready.push(i);
    }

    CompiledRenderGraph result{};
    result.passes = std::move(declarations);
    result.dependencies = std::move(dependencies);
    std::vector<uint32_t> fullOrder;
    fullOrder.reserve(result.passes.size());
    while (!ready.empty()) {
        const uint32_t passIndex = ready.top();
        ready.pop();
        fullOrder.push_back(passIndex);
        for (uint32_t consumer : outgoing[passIndex]) {
            if (--indegree[consumer] == 0)
                ready.push(consumer);
        }
    }
    if (fullOrder.size() != result.passes.size())
        throw std::runtime_error("RenderGraph contains a dependency cycle");

    uint64_t hash = 0;
    for (uint32_t passIndex : fullOrder) {
        const bool active = result.passes[passIndex].active;
        (active ? result.executionOrder : result.culledPasses)
            .push_back(passIndex);
        for (char c : result.passes[passIndex].name)
            hashCombine(hash, static_cast<unsigned char>(c));
        hashCombine(hash, passIndex);
        hashCombine(hash, active);
    }
    for (const auto &dependency : result.dependencies) {
        hashCombine(hash, dependency.producer);
        hashCombine(hash, dependency.consumer);
        hashCombine(hash, dependency.resource);
    }
    result.topologyHash = hash;
    return result;
}

void RenderGraphExecutor::execute(
    const CompiledRenderGraph &compiled,
    const std::vector<std::unique_ptr<IRenderPass>> &passes,
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources,
    const VisibilityFrame &visibility, GpuPassProfiler *profiler,
    std::unordered_map<uint64_t, RenderGraphImageState> &states,
    std::unordered_map<uint64_t, RenderGraphBufferState> &bufferStates,
    RenderGraphDiagnostics &diagnostics) {
    diagnostics.automaticBarriers = 0;
    diagnostics.layoutBarriers = 0;
    diagnostics.hazardBarriers = 0;
    std::vector<uint32_t> remainingNodes(passes.size(), 0);
    std::vector<bool> profilerStarted(passes.size(), false);
    for (uint32_t nodeIndex : compiled.executionOrder)
        ++remainingNodes[compiled.passes[nodeIndex].ownerPassIndex];
    for (uint32_t passIndex : compiled.executionOrder) {
        const RenderGraphPassDeclaration &declaration =
            compiled.passes.at(passIndex);
        IRenderPass &pass = *passes.at(declaration.ownerPassIndex);
        for (const RenderGraphImageUse &imageUse : declaration.images) {
            const RenderImageUsage &usage = imageUse.physical;
            if (usage.requiredLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                continue;
            transitionUsage(frame.cmd, resources, usage,
                            imageUse.subresource,
                            frame.frameIndex, usage.requiredLayout,
                            stateFor(usage.access,
                                     usage.requiredLayout),
                            states, &diagnostics);
        }
        for (const RenderGraphImportedImageUse &imageUse :
             declaration.importedImages) {
            if (imageUse.kind == RgImportedImageKind::Swapchain) {
                transitionImportedImage(
                    frame.cmd, frame.swapchainImage, imageUse.access,
                    imageUse.subresource, imageUse.requiredLayout, states,
                    &diagnostics);
            }
        }
        for (const RenderGraphBufferUse &bufferUse : declaration.buffers) {
            if (bufferUse.frameSlot == UINT32_MAX ||
                bufferUse.frameSlot == frame.frameIndex) {
                transitionBuffer(frame.cmd, bufferUse, bufferStates,
                                 &diagnostics);
            }
        }
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        VkRenderingAttachmentInfo depthAttachment{};
        VkRenderingAttachmentInfo stencilAttachment{};
        bool hasDepth = false;
        bool hasStencil = false;
        VkExtent2D renderingExtent{};
        uint32_t renderingLayers = 0;
        const auto mergeExtent = [&](VkExtent2D extent) {
            if (renderingExtent.width == 0) {
                renderingExtent = extent;
            } else if (renderingExtent.width != extent.width ||
                       renderingExtent.height != extent.height) {
                throw std::runtime_error(
                    declaration.name +
                    " has attachments with mismatched extents");
            }
        };
        const auto makeAttachment = [&](const RenderGraphAttachment &source) {
            VkRenderingAttachmentInfo result{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (source.importedSwapchain) {
                result.imageView = frame.swapchainImageView;
                mergeExtent(frame.swapchainExtent);
            } else {
                result.imageView = resources.attachmentView(
                    source.image, frame.frameIndex,
                    source.subresource.baseMipLevel,
                    source.subresource.baseArrayLayer,
                    source.subresource.layerCount,
                    source.subresource.aspectMask);
                mergeExtent(resources.mipExtent(
                    source.image, source.subresource.baseMipLevel));
            }
            result.imageLayout = source.layout;
            if (renderingLayers == 0) {
                renderingLayers = source.subresource.layerCount;
            } else if (renderingLayers != source.subresource.layerCount) {
                throw std::runtime_error(
                    declaration.name +
                    " has attachments with mismatched layer counts");
            }
            result.loadOp = source.loadOp;
            result.storeOp = source.storeOp;
            result.clearValue = source.clearValue;
            if (source.resolveImage.valid()) {
                result.resolveMode = source.resolveMode;
                result.resolveImageView = resources.attachmentView(
                    source.resolveImage, frame.frameIndex,
                    source.subresource.baseMipLevel,
                    source.subresource.baseArrayLayer,
                    source.subresource.layerCount,
                    source.subresource.aspectMask);
                result.resolveImageLayout = source.resolveLayout;
            }
            return result;
        };
        for (const RenderGraphAttachment &attachment :
             declaration.attachments) {
            VkRenderingAttachmentInfo info = makeAttachment(attachment);
            if (attachment.kind == RgAttachmentKind::Color) {
                colorAttachments.push_back(info);
            } else if (attachment.kind == RgAttachmentKind::Depth) {
                depthAttachment = info;
                hasDepth = true;
            } else {
                stencilAttachment = info;
                hasStencil = true;
            }
        }
        const bool dynamicRendering = !declaration.attachments.empty();
        if (dynamicRendering) {
            if (declaration.type != RgPassType::Graphics)
                throw std::runtime_error(
                    declaration.name +
                    " declares attachments on a non-graphics node");
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, renderingExtent};
            rendering.layerCount = renderingLayers;
            rendering.colorAttachmentCount =
                static_cast<uint32_t>(colorAttachments.size());
            rendering.pColorAttachments = colorAttachments.data();
            rendering.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
            rendering.pStencilAttachment =
                hasStencil ? &stencilAttachment : nullptr;
            vkCmdBeginRendering(frame.cmd, &rendering);
        }
        if (profiler && !profilerStarted[declaration.ownerPassIndex]) {
            profiler->beginPass(frame.cmd, frame.frameIndex,
                                declaration.groupId);
            profilerStarted[declaration.ownerPassIndex] = true;
        }
        if (frame.debugUtils) {
            ScopedGpuLabel label(*frame.debugUtils, frame.cmd,
                                 declaration.name);
            RenderGraphPassContext passContext{frame, resources};
            pass.recordNode(passContext, declaration.localNodeIndex,
                            visibility);
        } else {
            RenderGraphPassContext passContext{frame, resources};
            pass.recordNode(passContext, declaration.localNodeIndex,
                            visibility);
        }
        if (dynamicRendering)
            vkCmdEndRendering(frame.cmd);
        if (--remainingNodes[declaration.ownerPassIndex] == 0 && profiler)
            profiler->endPass(frame.cmd, frame.frameIndex,
                              declaration.groupId);
        for (const RenderGraphImageUse &imageUse : declaration.images) {
            const RenderImageUsage &usage = imageUse.physical;
            if (usage.finalLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                continue;
            if (!pass.managesDeclaredTransitionsInternally() &&
                usage.finalLayout != usage.requiredLayout) {
                RenderGraphImageState finalState =
                    stateFor(usage.access, usage.finalLayout);
                if (usage.finalLayout ==
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                    usage.finalLayout ==
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
                    finalState.stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    finalState.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                }
                transitionUsage(frame.cmd, resources, usage,
                                imageUse.subresource,
                                frame.frameIndex, usage.finalLayout,
                                finalState, states, &diagnostics);
            } else {
                const Image &image = usageImage(
                    resources, usage, frame.frameIndex);
                for (uint32_t mip = imageUse.subresource.baseMipLevel;
                     mip < imageUse.subresource.baseMipLevel +
                               imageUse.subresource.levelCount;
                     ++mip) {
                    for (uint32_t layer =
                             imageUse.subresource.baseArrayLayer;
                         layer < imageUse.subresource.baseArrayLayer +
                                     imageUse.subresource.layerCount;
                         ++layer) {
                        states[imageStateKey(
                            image.handle(), mip, layer,
                            imageUse.subresource.aspectMask)] =
                            stateFor(usage.access, usage.finalLayout);
                    }
                }
            }
        }
        for (const RenderGraphImportedImageUse &imageUse :
             declaration.importedImages) {
            if (imageUse.kind == RgImportedImageKind::Swapchain) {
                transitionImportedImage(
                    frame.cmd, frame.swapchainImage, imageUse.access,
                    imageUse.subresource, imageUse.finalLayout, states,
                    &diagnostics);
            }
        }
    }
}

RenderGraph::~RenderGraph() = default;

void RenderGraph::addPass(std::unique_ptr<IRenderPass> pass) {
    passes_.push_back(std::move(pass));
    compiledValid_ = false;
    compiledCache_.clear();
}

void RenderGraph::compile(const RenderResourceRegistry &resources,
                          const FrameRenderFeatures &features) {
    compiledFeatureKey_ = featureKey(passes_, resources, features);
    const auto cached = compiledCache_.find(compiledFeatureKey_);
    if (cached != compiledCache_.end()) {
        compiled_ = cached->second;
        compiledValid_ = true;
        diagnostics_ = makeDiagnostics(compiled_, resources);
        return;
    }
    compiled_ = RenderGraphCompiler::compile(passes_, resources, features);
    compiledCache_.emplace(compiledFeatureKey_, compiled_);
    compiledValid_ = true;
    diagnostics_ = makeDiagnostics(compiled_, resources);
    std::string order;
    for (uint32_t passIndex : compiled_.executionOrder) {
        if (!order.empty())
            order += " -> ";
        order += compiled_.passes[passIndex].name;
    }
    VKR_LOG_INFO("RenderGraph", "Compiled {} active / {} culled passes, {} dependencies, topology={:016x}: {}",
                 compiled_.executionOrder.size(),
                 compiled_.culledPasses.size(),
                 compiled_.dependencies.size(), compiled_.topologyHash,
                 order);
}

void RenderGraph::execute(const RenderFrameContext &frame,
                          const RenderResourceRegistry &resources,
                          const VisibilityFrame &visibility,
                          GpuPassProfiler *profiler) {
    for (const auto &pass : passes_)
        pass->prepareFrame(frame, resources, visibility);
    const uint64_t requestedKey = featureKey(passes_, resources, frame.features);
    if (!compiledValid_ || requestedKey != compiledFeatureKey_)
        compile(resources, frame.features);
    RenderGraphExecutor::execute(compiled_, passes_, frame, resources,
                                 visibility, profiler, imageStates_,
                                 bufferStates_,
                                 diagnostics_);
}

void RenderGraph::releaseSwapChainResources() {
    for (auto pass = passes_.rbegin(); pass != passes_.rend(); ++pass)
        (*pass)->releaseSwapChainResources();
}

void RenderGraph::releaseViewportResources() {
    for (auto pass = passes_.rbegin(); pass != passes_.rend(); ++pass)
        (*pass)->releaseViewportResources();
}

void RenderGraph::onViewportResize(const RenderResourceRegistry &resources) {
    for (auto &pass : passes_)
        pass->onViewportResize(resources);
    compiledValid_ = false;
    compiledCache_.clear();
    imageStates_.clear();
    bufferStates_.clear();
}

void RenderGraph::onSwapChainResize(const SwapChain &swapChain) {
    for (auto &pass : passes_)
        pass->onSwapChainResize(swapChain);
}

std::vector<GpuPassProfile> RenderGraph::passProfiles() const {
    std::vector<GpuPassProfile> profiles;
    profiles.reserve(passes_.size());
    for (uint32_t index = 0; index < passes_.size(); ++index) {
        profiles.push_back(
            {groupPassId(passes_[index]->name(), index),
             std::string(passes_[index]->name())});
    }
    return profiles;
}

std::string RenderGraph::toJson() const {
    const auto quote = [](const std::string &value) {
        std::string result = "\"";
        for (char c : value) {
            if (c == '\\' || c == '\"')
                result.push_back('\\');
            result.push_back(c);
        }
        result.push_back('\"');
        return result;
    };
    std::ostringstream out;
    out << "{\"topologyHash\":\"" << std::hex
        << diagnostics_.topologyHash << std::dec
        << "\",\"activePasses\":" << diagnostics_.activePasses
        << ",\"culledPasses\":" << diagnostics_.culledPasses
        << ",\"dependencyEdges\":" << diagnostics_.dependencyEdges
        << ",\"automaticBarriers\":" << diagnostics_.automaticBarriers
        << ",\"layoutBarriers\":" << diagnostics_.layoutBarriers
        << ",\"hazardBarriers\":" << diagnostics_.hazardBarriers
        << ",\"activeImageBytes\":" << diagnostics_.activeImageBytes
        << ",\"residentImageBytes\":" << diagnostics_.residentImageBytes
        << ",\"executionOrder\":[";
    for (size_t i = 0; i < diagnostics_.executionOrder.size(); ++i) {
        if (i)
            out << ',';
        out << quote(diagnostics_.executionOrder[i]);
    }
    out << "],\"culled\":[";
    for (size_t i = 0; i < diagnostics_.culledNames.size(); ++i) {
        if (i)
            out << ',';
        out << quote(diagnostics_.culledNames[i]);
    }
    out << "],\"resources\":[";
    for (size_t i = 0; i < diagnostics_.resources.size(); ++i) {
        if (i)
            out << ',';
        const auto &resource = diagnostics_.resources[i];
        out << "{\"index\":" << resource.index
            << ",\"name\":" << quote(resource.name)
            << ",\"lifetime\":" << quote(resource.lifetime)
            << ",\"versions\":" << resource.versions
            << ",\"residentBytes\":" << resource.residentBytes
            << ",\"initialLayout\":"
            << static_cast<int32_t>(resource.initialLayout)
            << ",\"finalLayout\":"
            << static_cast<int32_t>(resource.finalLayout)
            << ",\"producers\":[";
        for (size_t producer = 0; producer < resource.producers.size();
             ++producer) {
            if (producer)
                out << ',';
            out << quote(resource.producers[producer]);
        }
        out << "],\"consumers\":[";
        for (size_t consumer = 0; consumer < resource.consumers.size();
             ++consumer) {
            if (consumer)
                out << ',';
            out << quote(resource.consumers[consumer]);
        }
        out << "]}";
    }
    out << "],\"buffers\":[";
    for (size_t i = 0; i < diagnostics_.buffers.size(); ++i) {
        if (i)
            out << ',';
        const auto &buffer = diagnostics_.buffers[i];
        out << "{\"index\":" << buffer.index
            << ",\"nativeHandle\":\"0x" << std::hex
            << buffer.nativeHandle << std::dec << "\""
            << ",\"name\":" << quote(buffer.name)
            << ",\"lifetime\":" << quote(buffer.lifetime)
            << ",\"versions\":" << buffer.versions
            << ",\"declaredRangeBytes\":"
            << buffer.declaredRangeBytes << ",\"producers\":[";
        for (size_t producer = 0; producer < buffer.producers.size();
             ++producer) {
            if (producer)
                out << ',';
            out << quote(buffer.producers[producer]);
        }
        out << "],\"consumers\":[";
        for (size_t consumer = 0; consumer < buffer.consumers.size();
             ++consumer) {
            if (consumer)
                out << ',';
            out << quote(buffer.consumers[consumer]);
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string RenderGraph::toDot() const {
    std::ostringstream out;
    out << "digraph RenderGraph {\n  rankdir=LR;\n";
    for (uint32_t index = 0; index < compiled_.passes.size(); ++index) {
        const auto &pass = compiled_.passes[index];
        out << "  p" << index << " [label=\"" << pass.name
            << "\", style=filled, fillcolor=\""
            << (pass.active ? "#b7e4c7" : "#d9d9d9") << "\"];\n";
    }
    for (const auto &edge : compiled_.dependencies) {
        out << "  p" << edge.producer << " -> p" << edge.consumer;
        if (edge.resource != kInvalidRenderResource)
            out << " [label=\"image " << edge.resource << "\"]";
        out << ";\n";
    }
    out << "}\n";
    return out.str();
}

} // namespace vkr
