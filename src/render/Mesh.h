#pragma once
#include "core/Buffer.h"
#include "scene/Scene.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class UploadRecorder;

class Mesh {
  public:
    static std::unique_ptr<Mesh> fromOBJ(Device &device,
                                         UploadRecorder &upload,
                                         const std::string &path);

    Mesh(Device &device, UploadRecorder &upload, const void *vertexData,
         VkDeviceSize vertexSize, const uint32_t *indexData,
         uint32_t indexCount);
    ~Mesh() = default;

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    uint32_t indexCount() const { return indexCount_; }
    const Bounds &localBounds() const { return localBounds_; }

  private:
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    uint32_t                indexCount_ = 0;
    Bounds                  localBounds_;
};

} // namespace vkr
