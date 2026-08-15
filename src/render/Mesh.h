#pragma once
#include "core/Buffer.h"
#include "scene/SceneTypes.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class UploadRecorder;
class AccelerationStructure;

class Mesh {
  public:
    Mesh(Device &device, UploadRecorder &upload, const void *vertexData,
         VkDeviceSize vertexSize, const uint32_t *indexData,
         uint32_t indexCount, std::string debugName = {});
    ~Mesh();

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd, uint32_t firstInstance = 0) const;
    void drawIndirect(VkCommandBuffer cmd, VkBuffer indirectBuffer,
                      VkDeviceSize offset) const;

    uint32_t indexCount() const { return indexCount_; }
    uint32_t vertexCount() const { return vertexCount_; }
    const Bounds &localBounds() const { return localBounds_; }
    const AccelerationStructure *bottomLevelAccelerationStructure() const {
        return bottomLevelAccelerationStructure_.get();
    }
    VkDeviceAddress vertexDeviceAddress() const;
    VkDeviceAddress indexDeviceAddress() const;
    void releaseAccelerationBuildScratch();

  private:
    void buildBottomLevelAccelerationStructure(Device &device,
                                                VkCommandBuffer commandBuffer,
                                                const std::string &debugName);

    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    std::unique_ptr<AccelerationStructure>
        bottomLevelAccelerationStructure_;
    std::unique_ptr<Buffer> accelerationBuildScratch_;
    uint32_t                vertexCount_ = 0;
    uint32_t                indexCount_ = 0;
    Bounds                  localBounds_;
};

} // namespace vkr
