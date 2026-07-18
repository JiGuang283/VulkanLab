#pragma once

#include "GltfAsset.h"

#include <memory>
#include <string>

namespace vkr {

class Device;
class DescriptorAllocator;
class FallbackTextures;
class MaterialTemplate;
class UploadContext;
struct SceneLoadStats;

/// Loads a glTF 2.0 (.gltf/.glb) file into a self-contained GltfAsset:
/// decoded textures, materials with PBR factors, meshes (one per primitive)
/// and SceneObject instances. v1 uses identity transforms (Step 6 will add
/// node hierarchy), default samplers, and only baseColor textures.
class GltfLoader {
  public:
    struct Options {
        bool                     generateMissingNormals = true;
        uint32_t                 maxTextureSize = 2048; // 0 = Full resolution
        std::shared_ptr<FallbackTextures> fallbackTextures;
        SceneLoadStats          *loadStats = nullptr;
    };

    static GltfAsset load(const std::string &path, Device &device,
                          UploadContext &upload,
                          DescriptorAllocator  &descriptorAllocator,
                          std::shared_ptr<MaterialTemplate> materialTemplate,
                          const Options        &opts = {});
};

} // namespace vkr
