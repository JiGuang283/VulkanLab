#pragma once

#include <vulkan/vulkan.h>

#include "VulkanException.h"

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        const VkResult vkResult = (expr);                                      \
        if (vkResult != VK_SUCCESS) {                                          \
            throw ::vkr::VulkanException(vkResult, #expr, __FILE__, __LINE__); \
        }                                                                      \
    } while (0)
