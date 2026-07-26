#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include <vulkan/vulkan.h>

namespace vkr {

struct GpuDebugFunctionTable {
    PFN_vkSetDebugUtilsObjectNameEXT setObjectName = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT insertLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
};

class GpuDebugUtils {
  public:
    GpuDebugUtils() = default;
    GpuDebugUtils(VkDevice device, bool enabled);
    GpuDebugUtils(VkDevice device, GpuDebugFunctionTable functions);

    bool namingAvailable() const {
        return device_ != VK_NULL_HANDLE && functions_.setObjectName;
    }
    bool labelsAvailable() const {
        return functions_.beginLabel && functions_.insertLabel &&
               functions_.endLabel;
    }
    bool available() const {
        return namingAvailable() && labelsAvailable();
    }

    void setObjectName(VkObjectType type, uint64_t handle,
                       std::string_view name) const;

    template <typename Handle>
    void setObjectName(VkObjectType type, Handle handle,
                       std::string_view name) const {
        setObjectName(type, handleValue(handle), name);
    }

    bool beginLabel(
        VkCommandBuffer commandBuffer, std::string_view name,
        const std::array<float, 4> &color = {0.35f, 0.65f, 0.95f, 1.0f}) const;
    void insertLabel(
        VkCommandBuffer commandBuffer, std::string_view name,
        const std::array<float, 4> &color = {0.35f, 0.65f, 0.95f, 1.0f}) const;
    void endLabel(VkCommandBuffer commandBuffer) const;

  private:
    template <typename Handle> static uint64_t handleValue(Handle handle) {
        if constexpr (std::is_pointer_v<Handle>) {
            return static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(handle));
        } else {
            return static_cast<uint64_t>(handle);
        }
    }

    VkDevice device_ = VK_NULL_HANDLE;
    GpuDebugFunctionTable functions_{};
};

class ScopedGpuLabel {
  public:
    ScopedGpuLabel(
        const GpuDebugUtils &debugUtils, VkCommandBuffer commandBuffer,
        std::string_view name,
        const std::array<float, 4> &color = {0.35f, 0.65f, 0.95f, 1.0f});
    ~ScopedGpuLabel();

    ScopedGpuLabel(const ScopedGpuLabel &) = delete;
    ScopedGpuLabel &operator=(const ScopedGpuLabel &) = delete;

    ScopedGpuLabel(ScopedGpuLabel &&other) noexcept;
    ScopedGpuLabel &operator=(ScopedGpuLabel &&other) noexcept;

  private:
    void close();

    const GpuDebugUtils *debugUtils_ = nullptr;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    bool active_ = false;
};

} // namespace vkr
