#pragma once

#include "core/Buffer.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class AccelerationStructure {
  public:
    AccelerationStructure(Device &device,
                          VkAccelerationStructureTypeKHR type,
                          VkDeviceSize size, std::string debugName);
    ~AccelerationStructure();

    AccelerationStructure(const AccelerationStructure &) = delete;
    AccelerationStructure &operator=(const AccelerationStructure &) = delete;

    VkAccelerationStructureKHR handle() const { return handle_; }
    VkDeviceAddress deviceAddress() const { return deviceAddress_; }
    VkDeviceSize size() const { return storage_ ? storage_->size() : 0; }

  private:
    Device *device_ = nullptr;
    std::unique_ptr<Buffer> storage_;
    VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
    VkDeviceAddress deviceAddress_ = 0;
};

} // namespace vkr
