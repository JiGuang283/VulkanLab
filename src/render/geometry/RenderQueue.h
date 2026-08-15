#pragma once

#include "RenderItem.h"

#include <cstddef>
#include <glm/glm.hpp>
#include <vector>

namespace vkr {

class RenderQueue {
  public:
    void clear();
    void add(RenderItem command);
    void sortOpaque();
    void sortTransparent(const glm::vec3 &cameraPosition);

    const std::vector<RenderItem> &opaque() const { return opaque_; }
    const std::vector<RenderItem> &transparent() const {
        return transparent_;
    }
    std::vector<RenderItem> &mutableOpaque() { return opaque_; }
    std::vector<RenderItem> &mutableTransparent() {
        return transparent_;
    }

    size_t drawCount() const { return opaque_.size() + transparent_.size(); }
    size_t uniqueMaterialCount() const;
    size_t uniqueMeshCount() const;

  private:
    std::vector<RenderItem> opaque_;
    std::vector<RenderItem> transparent_;
};

} // namespace vkr

