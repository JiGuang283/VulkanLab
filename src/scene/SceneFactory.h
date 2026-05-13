#pragma once

#include <functional>
#include <memory>
#include <string>

namespace vkr {

class Device;
class DescriptorAllocator;
class FrameSync;
class Scene;

/// Constructs a scene given already-created core objects. The factory
/// captures by value anything it needs (paths, etc.).
///
/// Note: a Pipeline is intentionally NOT passed here. Scene factories create
/// material templates; Application builds the shared opaque pipeline from the
/// scene's primary template.
using SceneFactory = std::function<std::unique_ptr<Scene>(
    Device &, FrameSync &, DescriptorAllocator &)>;

struct SceneEntry {
    std::string  name;
    SceneFactory factory;
};

} // namespace vkr
