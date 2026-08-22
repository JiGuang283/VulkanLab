#pragma once

#include <RuntimeFeatures.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vulkan/vulkan.h>

namespace vkr {

class TracyGpuZone {
  public:
    TracyGpuZone();
    ~TracyGpuZone();

    TracyGpuZone(TracyGpuZone &&) noexcept;
    TracyGpuZone &operator=(TracyGpuZone &&) noexcept;

    TracyGpuZone(const TracyGpuZone &) = delete;
    TracyGpuZone &operator=(const TracyGpuZone &) = delete;

  private:
    friend class TracyProfiler;
    struct Impl;
    explicit TracyGpuZone(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class TracyProfiler {
  public:
    TracyProfiler(VkInstance instance, VkPhysicalDevice physicalDevice,
                  VkDevice device, VkQueue graphicsQueue,
                  uint32_t graphicsQueueFamily);
    ~TracyProfiler();

    TracyProfiler(const TracyProfiler &) = delete;
    TracyProfiler &operator=(const TracyProfiler &) = delete;

    bool compiled() const;
    bool connected() const;
    bool gpuAvailable() const;
    std::string_view version() const;

    void collect(VkCommandBuffer commandBuffer) const;
    TracyGpuZone beginGpuZone(VkCommandBuffer commandBuffer,
                              std::string_view name, uint32_t line,
                              const char *source,
                              const char *function) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr

#define VKL_PROFILE_DETAIL_CONCAT_INNER(a, b) a##b
#define VKL_PROFILE_DETAIL_CONCAT(a, b) VKL_PROFILE_DETAIL_CONCAT_INNER(a, b)

#if VKL_ENABLE_TRACY
#define VKL_PROFILE_GPU_ZONE(profiler, commandBuffer, name)                 \
    auto VKL_PROFILE_DETAIL_CONCAT(vklTracyGpuZone_, __LINE__) =            \
        (profiler).beginGpuZone((commandBuffer), (name), __LINE__,          \
                                __FILE__, __func__)
#else
#define VKL_PROFILE_GPU_ZONE(profiler, commandBuffer, name) ((void)0)
#endif
