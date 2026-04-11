#include <app.h>

void HelloTriangleApplication::createDepthResources() {
    VkFormat depthFormat = findDepthFormat();

    depthImage_ = std::make_unique<vkr::Image>(
        *device, swapChain_->extent().width, swapChain_->extent().height, 1,
        device->msaaSamples(), depthFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    depthImage_->createView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);

    transitionImageLayout(depthImage_->handle(), depthFormat,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);
}

VkFormat HelloTriangleApplication::findSuportedFormat(
    const std::vector<VkFormat> &candidates, VkImageTiling tiling,
    VkFormatFeatureFlags features) {
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), format,
                                            &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        } else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
                   (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error("failed to find supported format!");
}

VkFormat HelloTriangleApplication::findDepthFormat() {
    return findSuportedFormat({VK_FORMAT_D32_SFLOAT,
                               VK_FORMAT_D32_SFLOAT_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT},
                              VK_IMAGE_TILING_OPTIMAL,
                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

bool hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT;
}
