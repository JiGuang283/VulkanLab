#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/SwapChain.h"

#include <array>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

inline constexpr int MAX_FRAMES_IN_FLIGHT = 2;

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain,
             VkDeviceSize uniformBufferSize);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // ---- 帧循环 ----
    VkCommandBuffer beginFrame();
    void            endFrame();

    // ---- RenderPass 辅助 ----
    void beginRenderPass(VkCommandBuffer cmd);
    void endRenderPass(VkCommandBuffer cmd);

    // ---- 单次命令辅助 ----
    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer cmd);

    // ---- GPU 传输辅助 ----
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

    // ---- 交换链重建通知 ----
    void notifyResize() { framebufferResized_ = true; }

    // ---- 访问器 ----
    VkRenderPass  renderPass() const { return renderPass_; }
    VkCommandPool commandPool() const { return commandPool_; }
    uint32_t      frameIndex() const { return currentFrame_; }
    uint32_t      imageIndex() const { return currentImageIndex_; }

    // ---- per-frame UBO 访问器 ----
    void        *mappedUniformBuffer(uint32_t frameIndex) const;
    VkBuffer     uniformBufferHandle(uint32_t frameIndex) const;
    VkDeviceSize uniformBufferSize() const { return uniformBufferSize_; }

  private:
    struct FrameData {
        VkCommandBuffer         commandBuffer = VK_NULL_HANDLE;
        VkSemaphore             imageAvailable = VK_NULL_HANDLE;
        VkSemaphore             renderFinished = VK_NULL_HANDLE;
        VkFence                 inFlight = VK_NULL_HANDLE;
        std::unique_ptr<Buffer> uniformBuffer;
    };

    void createRenderPass();
    void createCommandPool();
    void createCommandBuffers();
    void createFramebuffers();
    void createSyncObjects();
    void createColorResources();
    void createDepthResources();
    void createUniformBuffers();

    void cleanupSwapChainResources();
    void recreateSwapChain();

    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates,
                                 VkImageTiling                tiling,
                                 VkFormatFeatureFlags         features);

    Device    *device_;
    SwapChain *swapChain_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> framebuffers_;

    std::unique_ptr<Image> colorImage_;
    std::unique_ptr<Image> depthImage_;

    std::array<FrameData, MAX_FRAMES_IN_FLIGHT> frames_;
    VkDeviceSize                                uniformBufferSize_ = 0;

    uint32_t currentFrame_ = 0;
    uint32_t currentImageIndex_ = 0;
    bool     framebufferResized_ = false;
    bool     frameInProgress_ = false;
};

} // namespace vkr
