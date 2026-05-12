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
/// Note: a Pipeline is intentionally NOT passed here. The opaque pipeline is
/// built by Application once from the first Material's descriptor layout, and
/// then shared across all scenes (all scenes currently use the same shaders
/// and therefore pipeline-layout-compatible descriptor sets).
using SceneFactory = std::function<std::unique_ptr<Scene>(
    Device &, FrameSync &, DescriptorAllocator &)>;

struct SceneEntry {
    std::string  name;
    SceneFactory factory;
};

} // namespace vkr
