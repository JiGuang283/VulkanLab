#pragma once

#include "RenderCommand.h"

#include <cstddef>
#include <vector>

namespace vkr {

class RenderQueue {
  public:
    void clear();
    void add(RenderCommand command);
    void sortOpaque();

    const std::vector<RenderCommand> &opaque() const { return opaque_; }

    size_t drawCount() const { return opaque_.size(); }
    size_t uniqueMaterialCount() const;
    size_t uniqueMeshCount() const;

  private:
    std::vector<RenderCommand> opaque_;
};

} // namespace vkr

