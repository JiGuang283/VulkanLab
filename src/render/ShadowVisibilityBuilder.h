#pragma once

#include "render/RenderItem.h"

#include <vector>

namespace vkr {

struct CullingSettings;
struct RenderView;
struct VisibilityFrame;

class ShadowVisibilityBuilder {
  public:
    static void build(const std::vector<RenderItem> &items,
                      const RenderView &view,
                      const CullingSettings &settings,
                      VisibilityFrame &frame);
};

} // namespace vkr
