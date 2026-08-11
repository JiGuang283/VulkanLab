#include "PointShadowPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/PunctualShadowSliceBuffer.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"
#include "render/pass/ShadowCasterDrawRecorder.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <glm/glm.hpp>
#include <string>

namespace vkr {

PointShadowPass::PointShadowPass(Device &device,
                                 const RenderResourceRegistry &resources,
                                 RenderImageHandle shadowDepth,
                                 DescriptorAllocator &descriptorAllocator,
                                 std::string vertPath,
                                 std::string opaqueFragPath,
                                 std::string maskFragPath)
    : device_(&device), shadowDepth_(shadowDepth),
      vertPath_(std::move(vertPath)),
      opaqueFragPath_(std::move(opaqueFragPath)),
      maskFragPath_(std::move(maskFragPath)) {
    sliceBuffer_ = std::make_unique<PunctualShadowSliceBuffer>(
        device, descriptorAllocator, kPointShadowLayers,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        "PointShadowSliceBuffer");

    createRenderPass(resources);
    createFramebuffers(resources);
}

PointShadowPass::~PointShadowPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_,
                            nullptr);
    sliceBuffer_.reset();
}

std::vector<RenderImageUsage> PointShadowPass::resourceUsages() const {
    return {{shadowDepth_, RenderImageAccess::DepthAttachmentWrite,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
}

void PointShadowPass::execute(const RenderFrameContext &frame,
                              const RenderResourceRegistry &resources,
                              const VisibilityFrame &visibility) {
    (void)resources;
    if (!frame.pipelineCache || !frame.view)
        return;
    const auto &punctualData = frame.view->shadow.punctual;

    VKL_PROFILE_ZONE("Record PointShadow");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "PointShadow");

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkViewport viewport{};
    viewport.width = static_cast<float>(kPointShadowMapSize);
    viewport.height = static_cast<float>(kPointShadowMapSize);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0},
                           {kPointShadowMapSize, kPointShadowMapSize}};

    if (punctualData.activePointCount == 0)
        return;

    for (uint32_t light = 0;
         light < punctualData.activePointCount; ++light) {
        const PointShadowData &point = punctualData.points[light];
        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
            PunctualShadowSlice slice{};
            slice.viewProjection = point.faceViewProjections[face];
            slice.lightPositionFar =
                glm::vec4(point.position, point.farPlane);
            const uint32_t sliceIndex =
                light * kPointShadowFaceCount + face;
            sliceBuffer_->write(frame.frameIndex, sliceIndex, slice);
        }
    }

    for (uint32_t lightIdx = 0;
         lightIdx < punctualData.activePointCount; ++lightIdx) {
        const auto &point = punctualData.points[lightIdx];
        if (!point.enabled)
            continue;

        ScopedGpuLabel lightLabel(
            device_->debugUtils(), frame.cmd,
            "Light " + std::to_string(lightIdx));

        for (uint32_t face = 0; face < kPointShadowFaceCount;
             ++face) {
            ScopedGpuLabel faceLabel(
                device_->debugUtils(), frame.cmd,
                "Face " + std::to_string(face));
            const uint32_t layer =
                lightIdx * kPointShadowFaceCount + face;

            VkRenderPassBeginInfo beginInfo{};
            beginInfo.sType =
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            beginInfo.renderPass = renderPass_;
            beginInfo.framebuffer =
                framebuffers_[layer];
            beginInfo.renderArea = {{0, 0},
                                    {kPointShadowMapSize,
                                     kPointShadowMapSize}};
            beginInfo.clearValueCount = 1;
            beginInfo.pClearValues = &clear;
            vkCmdBeginRenderPass(frame.cmd, &beginInfo,
                                 VK_SUBPASS_CONTENTS_INLINE);

            vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
            vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

            ShadowCasterDrawConfig drawConfig{};
            drawConfig.renderPass = renderPass_;
            drawConfig.sliceDescriptorLayout =
                sliceBuffer_->descriptorSetLayout();
            drawConfig.sliceDescriptorSet =
                sliceBuffer_->descriptorSet(frame.frameIndex);
            drawConfig.dynamicOffset =
                sliceBuffer_->dynamicOffset(layer);
            drawConfig.vertexShader = vertPath_;
            drawConfig.opaqueFragmentShader = opaqueFragPath_;
            drawConfig.maskFragmentShader = maskFragPath_;
            drawConfig.pipelinePrefix = "PointShadow";
            drawConfig.rasterDepthBias = false;
            ShadowCasterDrawRecorder::record(
                frame, visibility,
                visibility.pointShadowCasters[lightIdx][face],
                drawConfig);
            vkCmdEndRenderPass(frame.cmd);
        }
    }
}

void PointShadowPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    VkAttachmentDescription depth{};
    depth.format = resources.description(shadowDepth_).format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{
        0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags =
        VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags =
        VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depth;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount =
        static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info,
                                nullptr, &renderPass_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_RENDER_PASS, renderPass_,
        "Pass/PointShadow/RenderPass");
}

void PointShadowPass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const VkFormat format =
        resources.description(shadowDepth_).format;
    const Image &image = resources.image(shadowDepth_, 0);
    for (uint32_t layer = 0; layer < kPointShadowLayers; ++layer) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType =
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image.handle();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_DEPTH_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = layer;
            viewInfo.subresourceRange.layerCount = 1;
            VK_CHECK(vkCreateImageView(
                device_->logicalDevice(), &viewInfo, nullptr,
                &layerViews_[layer]));

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType =
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass_;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments =
                &layerViews_[layer];
            fbInfo.width = kPointShadowMapSize;
            fbInfo.height = kPointShadowMapSize;
            fbInfo.layers = 1;
            VK_CHECK(vkCreateFramebuffer(
                device_->logicalDevice(), &fbInfo, nullptr,
                &framebuffers_[layer]));
    }
}

void PointShadowPass::destroyFramebuffers() {
    for (uint32_t layer = 0; layer < kPointShadowLayers; ++layer) {
            if (framebuffers_[layer] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(
                    device_->logicalDevice(),
                    framebuffers_[layer], nullptr);
                framebuffers_[layer] = VK_NULL_HANDLE;
            }
            if (layerViews_[layer] != VK_NULL_HANDLE) {
                vkDestroyImageView(
                    device_->logicalDevice(),
                    layerViews_[layer], nullptr);
                layerViews_[layer] = VK_NULL_HANDLE;
            }
    }
}

} // namespace vkr
