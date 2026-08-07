#include "core/AccelerationStructure.h"

#include "core/Device.h"

#include <stdexcept>
#include <utility>

namespace vkr {

AccelerationStructure::AccelerationStructure(
    Device &device, VkAccelerationStructureTypeKHR type,
    VkDeviceSize size, std::string debugName)
    : device_(&device) {
    if (!device.rayQuerySupport().available || size == 0)
        throw std::invalid_argument(
            "AccelerationStructure requires Ray Query support and non-zero size");
    storage_ = std::make_unique<Buffer>(
        device, size,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
        debugName + "/Storage");
    handle_ = device.createAccelerationStructure(
        storage_->handle(), size, type, debugName);
    deviceAddress_ = device.accelerationStructureDeviceAddress(handle_);
    if (handle_ == VK_NULL_HANDLE || deviceAddress_ == 0)
        throw std::runtime_error(
            "Acceleration structure creation returned an invalid address");
}

AccelerationStructure::~AccelerationStructure() {
    if (device_ && handle_ != VK_NULL_HANDLE)
        device_->destroyAccelerationStructure(handle_);
}

} // namespace vkr
