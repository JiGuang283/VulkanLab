#pragma once

#include "SceneObject.h"
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Pipeline;

class Scene {
  public:
    void addObject(SceneObject obj);

    void render(VkCommandBuffer cmd, uint32_t frameIndex,
                Pipeline &pipeline) const;

    std::vector<SceneObject>       &objects() { return objects_; }
    const std::vector<SceneObject> &objects() const { return objects_; }

  private:
    std::vector<SceneObject> objects_;
};

} // namespace vkr
