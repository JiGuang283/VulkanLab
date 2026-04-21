#pragma once

#include <array>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class SwapChain;

inline constexpr int MAX_FRAMES_IN_FLIGHT = 2;

class FrameSync {
  public:
    struct FrameContext {
        VkCommandBuffer cmd;
        uint32_t        frameIndex; // 0..MAX_FRAMES_IN_FLIGHT-1
        uint32_t        imageIndex; // swapchain image index
    };

    FrameSync(Device &device, SwapChain &swapChain);
    ~FrameSync();

    FrameSync(const FrameSync &) = delete;
    FrameSync &operator=(const FrameSync &) = delete;

    /// 返回 nullopt 表示本帧应跳过（窗口最小化或交换链需重建）
    std::optional<FrameContext> beginFrame();
    void                        endFrame(const FrameContext &ctx);

    void notifyResize() { framebufferResized_ = true; }

    /// 如果为 true，调用方应执行交换链重建后调 onSwapChainRecreated()
    bool swapChainNeedsRecreation() const { return swapChainOutOfDate_; }
    void onSwapChainRecreated();

    // ---- 单次命令辅助（资源上传用）----
    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer cmd);

    // ---- GPU 传输辅助 ----
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

    // ---- 访问器 ----
    VkCommandPool commandPool() const { return commandPool_; }

  private:
    struct PerFrame {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        VkFence         inFlight = VK_NULL_HANDLE;
    };

    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    Device    *device_;
    SwapChain *swapChain_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    std::array<PerFrame, MAX_FRAMES_IN_FLIGHT> frames_;
    std::vector<VkSemaphore> renderFinished_; // 按 swapchain image index 索引

    uint32_t currentFrame_ = 0;
    bool     framebufferResized_ = false;
    bool     swapChainOutOfDate_ = false;
};

} // namespace vkr
