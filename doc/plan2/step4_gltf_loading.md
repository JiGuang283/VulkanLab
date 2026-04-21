# 第四步：glTF 模型加载

## 目标

在保留 OBJ 加载的同时增加 glTF 2.0 加载能力，可加载 `.glb` / `.gltf` 文件的**几何数据**（顶点 + 索引），纹理和材质参数暂时走默认值。

## 依赖

- [tinygltf](https://github.com/syoyo/tinygltf)：header-only，仅需 1 个 `.h` + 1 个 `.cpp`。
- tinygltf 内部依赖 `stb_image.h` 和 `nlohmann/json.hpp`（均已 inline 在 tinygltf 头文件中，或自选外部版本）。

## 改动清单

### A. 引入 tinygltf

```
external/
  tinygltf/
    tiny_gltf.h           ← 从 GitHub release 下载
```

新建 `src/tiny_gltf.cpp`：
```cpp
// 因项目已定义 STB_IMAGE_IMPLEMENTATION，此处只开 tinygltf
#define TINYGLTF_NO_STB_IMAGE_WRITE    // 不需要写图片
#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"
```

> **注意**：项目现有 `stb_image.cpp` 已定义 `STB_IMAGE_IMPLEMENTATION`。在 `tiny_gltf.cpp` 中加 `#define TINYGLTF_NO_STB_IMAGE` 避免重复定义。如果 tinygltf 版本不支持此宏，则把 stb_image 定义移到 tinygltf .cpp 内统一管理。

CMakeLists.txt 增加 include path：
```cmake
target_include_directories(VulkanLab PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/external/tinygltf   # ← 新增
)
```

### B. 新建 `src/render/GltfLoader.h / .cpp`

```cpp
// GltfLoader.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include "render/Vertex.h"

namespace vkr {

class Device;
class Renderer;
class Mesh;

/// glTF 加载结果（单 primitive → 单 Mesh）
struct GltfPrimitive {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

/// 从 glTF 文件加载所有 mesh primitive
/// 第一阶段：只提取 POSITION / NORMAL / TEXCOORD_0
/// color 统一白色，无法提取 texCoord 的 prim 给 (0,0)
class GltfLoader {
  public:
    /// 加载 .gltf 或 .glb
    static std::vector<GltfPrimitive> load(const std::string &path);

    /// 加载后直接创建 Mesh 对象（staging → device local）
    static std::vector<std::shared_ptr<Mesh>>
    loadMeshes(const std::string &path, Device &device, Renderer &renderer);
};

} // namespace vkr
```

```cpp
// GltfLoader.cpp
#include "GltfLoader.h"
#include "render/Mesh.h"
#include "core/Device.h"
#include "render/Renderer.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

#include <stdexcept>
#include <iostream>

namespace vkr {

// ---------- 辅助：从 accessor 读取 float 数据 ----------
static const float *getBufferFloat(const tinygltf::Model &model,
                                   int accessorIdx, size_t &count)
{
    const auto &acc    = model.accessors[accessorIdx];
    const auto &view   = model.bufferViews[acc.bufferView];
    const auto &buffer = model.buffers[view.buffer];
    count = acc.count;
    return reinterpret_cast<const float *>(
        buffer.data.data() + view.byteOffset + acc.byteOffset);
}

std::vector<GltfPrimitive> GltfLoader::load(const std::string &path)
{
    tinygltf::Model    model;
    tinygltf::TinyGLTF loader;
    std::string        err, warn;

    bool ok = false;
    if (path.ends_with(".glb")) {
        ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    } else {
        ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }
    if (!ok) throw std::runtime_error("glTF load failed: " + err);

    std::vector<GltfPrimitive> result;

    for (const auto &mesh : model.meshes) {
        for (const auto &prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES &&
                prim.mode != -1 /* default */)
                continue;

            GltfPrimitive gp;

            // ---- POSITION（必需）----
            auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end()) continue;
            size_t vertexCount = 0;
            const float *posData = getBufferFloat(model, posIt->second, vertexCount);

            // ---- TEXCOORD_0（可选）----
            const float *uvData = nullptr;
            size_t uvCount = 0;
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end()) {
                uvData = getBufferFloat(model, uvIt->second, uvCount);
            }

            // 填充顶点
            gp.vertices.resize(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                Vertex &v = gp.vertices[i];
                v.pos      = {posData[i * 3 + 0], posData[i * 3 + 1], posData[i * 3 + 2]};
                v.color    = {1.0f, 1.0f, 1.0f};
                v.texCoord = uvData
                    ? glm::vec2{uvData[i * 2 + 0], uvData[i * 2 + 1]}
                    : glm::vec2{0.0f, 0.0f};
            }

            // ---- 索引 ----
            if (prim.indices >= 0) {
                const auto &acc    = model.accessors[prim.indices];
                const auto &view   = model.bufferViews[acc.bufferView];
                const auto &buffer = model.buffers[view.buffer];
                const uint8_t *base = buffer.data.data() + view.byteOffset + acc.byteOffset;

                gp.indices.resize(acc.count);
                for (size_t i = 0; i < acc.count; ++i) {
                    switch (acc.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        gp.indices[i] = reinterpret_cast<const uint16_t *>(base)[i];
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        gp.indices[i] = reinterpret_cast<const uint32_t *>(base)[i];
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        gp.indices[i] = base[i];
                        break;
                    default:
                        throw std::runtime_error("Unsupported index type");
                    }
                }
            } else {
                // 无索引 → 生成 0, 1, 2, ...
                gp.indices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i)
                    gp.indices[i] = static_cast<uint32_t>(i);
            }

            result.push_back(std::move(gp));
        }
    }
    return result;
}

std::vector<std::shared_ptr<Mesh>>
GltfLoader::loadMeshes(const std::string &path, Device &device, Renderer &renderer)
{
    auto primitives = load(path);
    std::vector<std::shared_ptr<Mesh>> meshes;
    meshes.reserve(primitives.size());
    for (auto &p : primitives) {
        meshes.push_back(std::make_shared<Mesh>(device, renderer,
                                                 p.vertices, p.indices));
    }
    return meshes;
}

} // namespace vkr
```

### C. 修改 `Application.cpp` —— 按文件后缀选择加载

```cpp
#include "render/GltfLoader.h"

void Application::loadModel() {
    const auto &path = Config::modelPath;
    if (path.ends_with(".gltf") || path.ends_with(".glb")) {
        auto meshes = GltfLoader::loadMeshes(path, *device_, *renderer_);
        // 第一阶段：把每个 primitive 加为一个 SceneObject
        for (auto &m : meshes) {
            scene_.add({m, material_, glm::mat4(1.0f)});
        }
    } else {
        // 原有 OBJ 路径
        auto mesh = Mesh::fromOBJ(Config::modelPath, *device_, *renderer_);
        scene_.add({mesh, material_, glm::mat4(1.0f)});
    }
}
```

### D. Config.h 更新

```cpp
// 只需改 modelPath 即可切换模型
inline const std::string modelPath = "models/DamagedHelmet.glb";
```

### E. 测试素材

下载 Khronos glTF-Sample-Assets 中的模型（如 `DamagedHelmet.glb`、`Box.glb`）放到 `models/` 目录，测试几何是否正确。

## 验证

1. **编译通过**，无 STB_IMAGE 重复定义
2. **加载 .glb**：模型几何正确显示（纹理用当前默认纹理）
3. **保持 .obj 回退**：改回 OBJ 路径仍能正常工作
4. **多 primitive**：加载含多 primitive 的模型，确认全部 primitive 可见
5. **错误处理**：传入不存在的文件 → 抛异常并提示路径

## 局限（留给后续）

- 不解析 glTF material / texture → 后续 step5 Material 解耦后顺带接入
- 不处理 node hierarchy / transform → 所有 primitive 在世界原点
- 不处理 skin / animation

## 文件变更总结

```
新增：external/tinygltf/tiny_gltf.h
新增：src/tiny_gltf.cpp
新增：src/render/GltfLoader.h
新增：src/render/GltfLoader.cpp
修改：CMakeLists.txt              (include path)
修改：src/app/Application.cpp     (loadModel 分支)
修改：src/app/Config.h            (modelPath 可选)
```
