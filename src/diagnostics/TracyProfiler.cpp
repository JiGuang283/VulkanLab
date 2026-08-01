#include "TracyProfiler.h"

#include "Profiling.h"

#include <tracy/TracyVulkan.hpp>

#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {

struct TracyGpuZone::Impl {
    Impl(TracyVkCtx context, VkCommandBuffer commandBuffer,
         std::string_view name)
        : scope(context, __LINE__, __FILE__, std::strlen(__FILE__),
                __func__, std::strlen(__func__), name.data(), name.size(),
                commandBuffer, true) {}

    tracy::VkCtxScope scope;
};

struct TracyProfiler::Impl {
    VkDevice device = VK_NULL_HANDLE;
    TracyVkCtx gpuContext = nullptr;
};

TracyGpuZone::TracyGpuZone() = default;
TracyGpuZone::TracyGpuZone(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
TracyGpuZone::~TracyGpuZone() = default;
TracyGpuZone::TracyGpuZone(TracyGpuZone &&) noexcept = default;
TracyGpuZone &TracyGpuZone::operator=(TracyGpuZone &&) noexcept = default;

TracyProfiler::TracyProfiler(VkInstance, VkPhysicalDevice physicalDevice,
                             VkDevice device, VkQueue graphicsQueue,
                             uint32_t graphicsQueueFamily)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                              nullptr);
    if (graphicsQueueFamily >= familyCount)
        return;
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                              families.data());
    if (families[graphicsQueueFamily].timestampValidBits == 0)
        return;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) !=
        VK_SUCCESS) {
        return;
    }

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer) !=
        VK_SUCCESS) {
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    impl_->gpuContext =
        TracyVkContext(physicalDevice, device, graphicsQueue, commandBuffer);
    constexpr char kContextName[] = "VulkanLab Graphics";
    TracyVkContextName(impl_->gpuContext, kContextName,
                       sizeof(kContextName) - 1);
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    vkDestroyCommandPool(device, commandPool, nullptr);
}

TracyProfiler::~TracyProfiler() {
    if (impl_ && impl_->gpuContext)
        TracyVkDestroy(impl_->gpuContext);
}

bool TracyProfiler::compiled() const { return true; }
bool TracyProfiler::connected() const { return profileConnected(); }
bool TracyProfiler::gpuAvailable() const {
    return impl_ && impl_->gpuContext != nullptr;
}
std::string_view TracyProfiler::version() const { return "0.13.1"; }

void TracyProfiler::collect(VkCommandBuffer commandBuffer) const {
    if (gpuAvailable())
        TracyVkCollect(impl_->gpuContext, commandBuffer);
}

TracyGpuZone TracyProfiler::beginGpuZone(
    VkCommandBuffer commandBuffer, std::string_view name) const {
    if (!gpuAvailable())
        return {};
    return TracyGpuZone(std::make_unique<TracyGpuZone::Impl>(
        impl_->gpuContext, commandBuffer, name));
}

} // namespace vkr
