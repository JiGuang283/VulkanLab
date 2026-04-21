#pragma once
#include "core/Buffer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class FrameSync;

class Mesh {
  public:
    static std::unique_ptr<Mesh> fromOBJ(Device &device, FrameSync &frameSync,
                                         const std::string &path);

    Mesh(Device &device, FrameSync &frameSync, const void *vertexData,
         VkDeviceSize vertexSize, const uint32_t *indexData,
         uint32_t indexCount);
    ~Mesh() = default;

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    uint32_t indexCount() const { return indexCount_; }

  private:
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    uint32_t                indexCount_ = 0;
};

} // namespace vkr
