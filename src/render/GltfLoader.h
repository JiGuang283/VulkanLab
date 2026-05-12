#pragma once

#include "GltfAsset.h"

#include <memory>
#include <string>

namespace vkr {

class Device;
class DescriptorAllocator;
class FrameSync;
class Texture;
struct PipelineConfig;

/// Loads a glTF 2.0 (.gltf/.glb) file into a self-contained GltfAsset:
/// decoded textures, materials with PBR factors, meshes (one per primitive)
/// and SceneObject instances. v1 uses identity transforms (Step 6 will add
/// node hierarchy), default samplers, and only baseColor textures.
class GltfLoader {
  public:
    struct Options {
        bool                     generateMissingNormals = true;
        std::shared_ptr<Texture> fallbackWhite;
    };

    static GltfAsset load(const std::string &path, Device &device,
                          FrameSync &frameSync,
                          DescriptorAllocator  &descriptorAllocator,
                          const PipelineConfig &baseConfig,
                          const Options        &opts = {});
};

} // namespace vkr
