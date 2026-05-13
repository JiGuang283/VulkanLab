#pragma once

#include "IRenderPass.h"
#include "core/Image.h"

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class FrameSync;
class RenderQueue;
class SwapChain;
struct RenderFrameContext;

class MainForwardPass final : public IRenderPass {
  public:
    MainForwardPass(Device &device, SwapChain &swapChain, FrameSync &frameSync);
    ~MainForwardPass() override;

    MainForwardPass(const MainForwardPass &) = delete;
    MainForwardPass &operator=(const MainForwardPass &) = delete;

    std::string_view name() const override { return "MainForwardPass"; }
    void onResize(const SwapChain &swapChain) override;
    void execute(const RenderFrameContext &frame,
                 const RenderQueue &queue) override;

    VkRenderPass renderPass() const { return renderPass_; }

  private:
    void createRenderPass();
    void createFramebuffers();
    void createColorResources();
    void createDepthResources();
    void cleanupSwapChainResources();

    void begin(VkCommandBuffer cmd, uint32_t imageIndex);
    void end(VkCommandBuffer cmd);
    void drawQueue(const RenderFrameContext &frame, const RenderQueue &queue);

    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates,
                                 VkImageTiling                tiling,
                                 VkFormatFeatureFlags         features);

    Device    *device_ = nullptr;
    SwapChain *swapChain_ = nullptr;
    FrameSync *frameSync_ = nullptr;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    std::unique_ptr<Image> colorImage_;
    std::unique_ptr<Image> depthImage_;
};

} // namespace vkr
