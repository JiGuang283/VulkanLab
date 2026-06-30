#pragma once

#include "RenderCommand.h"

#include <cstddef>
#include <glm/glm.hpp>
#include <vector>

namespace vkr {

class RenderQueue {
  public:
    void clear();
    void add(RenderCommand command);
    void sortOpaque();
    void sortTransparent(const glm::vec3 &cameraPosition);

    const std::vector<RenderCommand> &opaque() const { return opaque_; }
    const std::vector<RenderCommand> &transparent() const {
        return transparent_;
    }

    size_t drawCount() const { return opaque_.size() + transparent_.size(); }
    size_t uniqueMaterialCount() const;
    size_t uniqueMeshCount() const;

  private:
    std::vector<RenderCommand> opaque_;
    std::vector<RenderCommand> transparent_;
};

} // namespace vkr

