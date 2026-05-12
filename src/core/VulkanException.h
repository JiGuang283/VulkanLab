#pragma once

#include <stdexcept>

#include <vulkan/vulkan.h>

namespace vkr {

const char *vkResultName(VkResult result);

class VulkanException : public std::runtime_error {
  public:
    VulkanException(VkResult result, const char *expression, const char *file,
                    int line);

    VkResult result() const { return result_; }

  private:
    VkResult result_ = VK_SUCCESS;
};

} // namespace vkr