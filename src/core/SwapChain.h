#pragma once
#include <functional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

using ExtentProvider = std::function<VkExtent2D()>;

class SwapChain {
  public:
    SwapChain(Device &device, VkSurfaceKHR surface, ExtentProvider getExtent);
    ~SwapChain();

    SwapChain(const SwapChain &) = delete;
    SwapChain &operator=(const SwapChain &) = delete;

    // 重建交换链（窗口大小改变时调用）
    void recreate();

    VkSwapchainKHR                  handle() const { return swapChain_; }
    VkFormat                        imageFormat() const { return imageFormat_; }
    VkExtent2D                      extent() const { return extent_; }
    const std::vector<VkImage>     &images() const { return images_; }
    VkImage image(uint32_t index) const { return images_.at(index); }
    const std::vector<VkImageView> &imageViews() const { return imageViews_; }
    uint32_t                        imageCount() const {
        return static_cast<uint32_t>(images_.size());
    }
    bool captureSupported() const { return captureSupported_; }
    const std::string &captureUnsupportedReason() const {
        return captureUnsupportedReason_;
    }

  private:
    void createSwapChain();
    void createImageViews();
    void cleanup();

    // 选择策略
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR> &availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR> &availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

    // ImageView 辅助
    VkImageView createImageView(VkImage image, VkFormat format,
                                VkImageAspectFlags aspectFlags,
                                uint32_t           mipLevels);

    Device        *device_ = nullptr;
    VkSurfaceKHR   surface_ = VK_NULL_HANDLE; // 非拥有，VulkanContext 管理
    ExtentProvider getExtent_;                // 回调获取窗口尺寸

    VkSwapchainKHR           swapChain_ = VK_NULL_HANDLE;
    VkFormat                 imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               extent_ = {0, 0};
    bool                     captureSupported_ = false;
    std::string              captureUnsupportedReason_;
    std::vector<VkImage>     images_;     // Vulkan 拥有，不手动销毁
    std::vector<VkImageView> imageViews_; // 需要手动销毁
};

} // namespace vkr
