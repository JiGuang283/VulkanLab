#include "GpuDebugUtils.h"

namespace vkr {

GpuDebugUtils::GpuDebugUtils(VkDevice, bool) {}

GpuDebugUtils::GpuDebugUtils(VkDevice, GpuDebugFunctionTable) {}

void GpuDebugUtils::setObjectName(VkObjectType, uint64_t,
                                  std::string_view) const {}

bool GpuDebugUtils::beginLabel(
    VkCommandBuffer, std::string_view,
    const std::array<float, 4> &) const {
    return false;
}

void GpuDebugUtils::insertLabel(
    VkCommandBuffer, std::string_view,
    const std::array<float, 4> &) const {}

void GpuDebugUtils::endLabel(VkCommandBuffer) const {}

ScopedGpuLabel::ScopedGpuLabel(
    const GpuDebugUtils &, VkCommandBuffer, std::string_view,
    const std::array<float, 4> &) {}

ScopedGpuLabel::~ScopedGpuLabel() = default;

ScopedGpuLabel::ScopedGpuLabel(ScopedGpuLabel &&other) noexcept
    : debugUtils_(other.debugUtils_),
      commandBuffer_(other.commandBuffer_), active_(other.active_) {
    other.debugUtils_ = nullptr;
    other.commandBuffer_ = VK_NULL_HANDLE;
    other.active_ = false;
}

ScopedGpuLabel &
ScopedGpuLabel::operator=(ScopedGpuLabel &&other) noexcept {
    if (this != &other) {
        close();
        debugUtils_ = other.debugUtils_;
        commandBuffer_ = other.commandBuffer_;
        active_ = other.active_;
        other.debugUtils_ = nullptr;
        other.commandBuffer_ = VK_NULL_HANDLE;
        other.active_ = false;
    }
    return *this;
}

void ScopedGpuLabel::close() {
    debugUtils_ = nullptr;
    commandBuffer_ = VK_NULL_HANDLE;
    active_ = false;
}

} // namespace vkr
