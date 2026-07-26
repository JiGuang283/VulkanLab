#include "GpuDebugUtils.h"

#include <string>

namespace vkr {

namespace {

GpuDebugFunctionTable loadFunctions(VkDevice device, bool enabled) {
    GpuDebugFunctionTable functions;
    if (!enabled || device == VK_NULL_HANDLE)
        return functions;
    functions.setObjectName =
        reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
    functions.beginLabel =
        reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
    functions.insertLabel =
        reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdInsertDebugUtilsLabelEXT"));
    functions.endLabel =
        reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
    return functions;
}

VkDebugUtilsLabelEXT makeLabel(std::string_view name,
                               const std::array<float, 4> &color,
                               std::string &storage) {
    storage.assign(name.data(), name.size());
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = storage.c_str();
    for (size_t index = 0; index < color.size(); ++index)
        label.color[index] = color[index];
    return label;
}

} // namespace

GpuDebugUtils::GpuDebugUtils(VkDevice device, bool enabled)
    : GpuDebugUtils(device, loadFunctions(device, enabled)) {}

GpuDebugUtils::GpuDebugUtils(VkDevice device,
                             GpuDebugFunctionTable functions)
    : device_(device), functions_(functions) {}

void GpuDebugUtils::setObjectName(VkObjectType type, uint64_t handle,
                                  std::string_view name) const {
    if (!namingAvailable() || handle == 0 || name.empty())
        return;
    std::string storage(name);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = storage.c_str();
    functions_.setObjectName(device_, &info);
}

bool GpuDebugUtils::beginLabel(
    VkCommandBuffer commandBuffer, std::string_view name,
    const std::array<float, 4> &color) const {
    if (!labelsAvailable() || commandBuffer == VK_NULL_HANDLE || name.empty())
        return false;
    std::string storage;
    const VkDebugUtilsLabelEXT label = makeLabel(name, color, storage);
    functions_.beginLabel(commandBuffer, &label);
    return true;
}

void GpuDebugUtils::insertLabel(
    VkCommandBuffer commandBuffer, std::string_view name,
    const std::array<float, 4> &color) const {
    if (!labelsAvailable() || commandBuffer == VK_NULL_HANDLE || name.empty())
        return;
    std::string storage;
    const VkDebugUtilsLabelEXT label = makeLabel(name, color, storage);
    functions_.insertLabel(commandBuffer, &label);
}

void GpuDebugUtils::endLabel(VkCommandBuffer commandBuffer) const {
    if (!labelsAvailable() || commandBuffer == VK_NULL_HANDLE)
        return;
    functions_.endLabel(commandBuffer);
}

ScopedGpuLabel::ScopedGpuLabel(
    const GpuDebugUtils &debugUtils, VkCommandBuffer commandBuffer,
    std::string_view name, const std::array<float, 4> &color)
    : debugUtils_(&debugUtils), commandBuffer_(commandBuffer),
      active_(debugUtils.beginLabel(commandBuffer, name, color)) {}

ScopedGpuLabel::~ScopedGpuLabel() { close(); }

ScopedGpuLabel::ScopedGpuLabel(ScopedGpuLabel &&other) noexcept
    : debugUtils_(std::exchange(other.debugUtils_, nullptr)),
      commandBuffer_(std::exchange(other.commandBuffer_, VK_NULL_HANDLE)),
      active_(std::exchange(other.active_, false)) {}

ScopedGpuLabel &
ScopedGpuLabel::operator=(ScopedGpuLabel &&other) noexcept {
    if (this == &other)
        return *this;
    close();
    debugUtils_ = std::exchange(other.debugUtils_, nullptr);
    commandBuffer_ =
        std::exchange(other.commandBuffer_, VK_NULL_HANDLE);
    active_ = std::exchange(other.active_, false);
    return *this;
}

void ScopedGpuLabel::close() {
    if (active_ && debugUtils_)
        debugUtils_->endLabel(commandBuffer_);
    active_ = false;
}

} // namespace vkr
