#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _vk_result = (expr);                                          \
        if (_vk_result != VK_SUCCESS) {                                        \
            throw std::runtime_error(                                          \
                std::string("Vulkan error ") +                                 \
                std::to_string(static_cast<int>(_vk_result)) + " at " +        \
                __FILE__ + ":" + std::to_string(__LINE__));                     \
        }                                                                      \
    } while (0)
