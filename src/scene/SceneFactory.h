#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vkr {

class Device;
class DescriptorAllocator;
class Scene;
class UploadContext;
struct SceneLoadStats;

struct SceneLoadContext {
    uint32_t maxTextureSize = 2048; // 0 = Full resolution
    SceneLoadStats *loadStats = nullptr;
};

/// Constructs a scene given already-created core objects. The factory
/// captures by value anything it needs (paths, etc.).
///
/// Note: a Pipeline is intentionally NOT passed here. Scene factories create
/// material templates; Application builds the shared opaque pipeline from the
/// scene's primary template.
using SceneFactory = std::function<std::unique_ptr<Scene>(
    Device &, UploadContext &, DescriptorAllocator &,
    const SceneLoadContext &)>;

struct SceneEntry {
    std::string  name;
    SceneFactory factory;
};

} // namespace vkr
