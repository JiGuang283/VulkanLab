#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Image.h"
#include "core/SwapChain.h"

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             VkDeviceSize uniformBufferSize);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // ---- RenderPass 辅助 ----
    void beginRenderPass(VkCommandBuffer cmd, uint32_t imageIndex);
    void endRenderPass(VkCommandBuffer cmd);

    // ---- 交换链重建 ----
    void recreateSwapChain();

    // ---- 访问器 ----
    VkRenderPass renderPass() const { return renderPass_; }

    // ---- per-frame UBO 访问器 ----
    void        *mappedUniformBuffer(uint32_t frameIndex) const;
    VkBuffer     uniformBufferHandle(uint32_t frameIndex) const;
    VkDeviceSize uniformBufferSize() const { return uniformBufferSize_; }

  private:
    void createRenderPass();
    void createFramebuffers();
    void createColorResources();
    void createDepthResources();
    void createUniformBuffers();

    void cleanupSwapChainResources();

    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates,
                                 VkImageTiling                tiling,
                                 VkFormatFeatureFlags         features);

    Device    *device_;
    SwapChain *swapChain_;
    FrameSync *frameSync_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> framebuffers_;

    std::unique_ptr<Image> colorImage_;
    std::unique_ptr<Image> depthImage_;

    std::vector<std::unique_ptr<Buffer>> uniformBuffers_;
    VkDeviceSize                         uniformBufferSize_ = 0;
};

} // namespace vkr
