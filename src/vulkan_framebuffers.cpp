#include "app.h"

void HelloTriangleApplication::createFramebuffers() {
    swapChainFramebuffers.resize(swapChain_->imageViews().size());

    for (size_t i = 0; i < swapChain_->imageViews().size(); i++) {
        std::array<VkImageView, 3> attachments = {colorImage_->imageView(),
                                                  depthImage_->imageView(),
                                                  swapChain_->imageViews()[i]};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount =
            static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapChain_->extent().width;
        framebufferInfo.height = swapChain_->extent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device->logicalDevice(), &framebufferInfo,
                                nullptr,
                                &swapChainFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create framebuffer!");
        }
    }
}
