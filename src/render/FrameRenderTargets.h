#pragma once

#include "core/FrameSync.h"

#include <array>
#include <memory>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class Image;

struct FrameRenderTarget {
    std::unique_ptr<Image> hdrColor;
    std::unique_ptr<Image> hdrMsaaColor;
    std::unique_ptr<Image> depth;
    std::unique_ptr<Image> shadowDepth;
};

class FrameRenderTargets {
  public:
    FrameRenderTargets(Device &device, VkExtent2D extent);
    ~FrameRenderTargets();

    FrameRenderTargets(const FrameRenderTargets &) = delete;
    FrameRenderTargets &operator=(const FrameRenderTargets &) = delete;

    void releaseExtentTargets();
    void recreateExtentTargets(VkExtent2D extent);

    FrameRenderTarget &frame(uint32_t frameIndex) {
        return frames_.at(frameIndex);
    }
    const FrameRenderTarget &frame(uint32_t frameIndex) const {
        return frames_.at(frameIndex);
    }

    VkExtent2D extent() const { return extent_; }
    VkFormat hdrFormat() const { return hdrFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkFormat shadowDepthFormat() const { return shadowDepthFormat_; }
    VkSampleCountFlagBits samples() const { return samples_; }
    VkSampler hdrSampler() const { return hdrSampler_; }
    VkSampler shadowSampler() const { return shadowSampler_; }

  private:
    VkFormat chooseHdrFormat() const;
    VkFormat chooseDepthFormat(bool sampled) const;
    VkSampleCountFlagBits chooseSamples() const;
    void createSamplers();
    void createShadowTargets();

    Device *device_ = nullptr;
    VkExtent2D extent_{};
    VkFormat hdrFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat shadowDepthFormat_ = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkSampler hdrSampler_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    std::array<FrameRenderTarget, MAX_FRAMES_IN_FLIGHT> frames_;
};

} // namespace vkr
