#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vkr {

class Device;
class FrameSync;
class Mesh;

/// Loads geometry from a glTF 2.0 (.gltf / .glb) file.
/// Only POSITION and TEXCOORD_0 attributes are extracted; normals, materials,
/// and textures are ignored in this stage.  Each triangle primitive becomes
/// one Mesh object with device-local vertex + index buffers.
class GltfLoader {
  public:
    /// Returns one Mesh per triangle primitive found in the file.
    /// Throws std::runtime_error on load failure or if no primitives exist.
    static std::vector<std::unique_ptr<Mesh>>
    load(const std::string &path, Device &device, FrameSync &frameSync);
};

} // namespace vkr
