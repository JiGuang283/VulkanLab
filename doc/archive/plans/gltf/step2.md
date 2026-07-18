# Step 2 具体执行计划

> 上游：[implementation_steps.md#step-2](./implementation_steps.md) · 前置 commit：Step 1 (`03f1dff`)
> 产出：单个 commit，把 `Vertex.color` 替换为 `Vertex.normal`，同步 shader 与所有写入 `vertex.color` 的地方。
> 关键约束：**一次性**提交 ×5 文件；中途**不能**只改半边，否则 shader 与顶点布局不匹配会崩。

---

## 2.1 影响范围盘点（已探测）

`rg` 结果显示写 `vertex.color` / `v.color` 的共 **2 处**（Step 2 实施前计划书未覆盖 GltfLoader）：

| 文件 | 行 | 语句 | 本次动作 |
| --- | --- | --- | --- |
| [src/render/Vertex.h](../../../../src/render/Vertex.h) | 17 / 52 / 64 | `glm::vec3 color;` + `==` + `hash` | `color` → `normal` |
| [src/render/Vertex.h](../../../../src/render/Vertex.h) | 38–41 | attribute location 1 `offsetof(Vertex, color)` | `offsetof(Vertex, normal)` |
| [src/render/Mesh.cpp](../../../../src/render/Mesh.cpp) | 85 | `vertex.color = {1,1,1};` | 改为读 `attrib.normals` / fallback |
| [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) | 108 | `v.color = {1,1,1};` | 改为读 `NORMAL` accessor / fallback；**此时先最小改** |
| `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`） | — | `inColor` / `fragColor` | `inNormal` / `fragNormalWS` |
| `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`） | — | 读 `fragColor`（未使用）+ 吐贴图 | 读 `fragNormalWS`，做软光照 |

**不在本步范围**：Renderer/SwapChain/Pipeline 里的 `color*` 命名全是 Vulkan 附件/混合相关，与 `Vertex.color` 同名不同义，**不动**。

### Step 2 与 Step 5 的衔接

GltfLoader 的 `v.color = {1,1,1}` 行当前（Step 2）只做**最简处理**：替换成 `v.normal = {0,1,0}`（或若能读到 `NORMAL` 则用之）。完整的"glTF NORMAL 属性解析"放在 Step 5；本步只保证 Step 1 commit 之后代码能继续跑，不引入新功能。

---

## 2.2 分阶段编辑清单（严格顺序）

> **注意**：所有编辑必须在一个 commit 内完成，但执行顺序按照下面来，能让每一次 Save 后被 LSP 发现的错误数最少。

### 2.2.1 [src/render/Vertex.h](../../../../src/render/Vertex.h)

3 处改动：
1. 字段：`glm::vec3 color;` → `glm::vec3 normal;`
2. attribute 数组：`offsetof(Vertex, color)` → `offsetof(Vertex, normal)`（format 保持 `VK_FORMAT_R32G32B32_SFLOAT` 不变）
3. `operator==`：`color == other.color` → `normal == other.normal`
4. `std::hash<vkr::Vertex>`：`hash<glm::vec3>()(vertex.color)` → `hash<glm::vec3>()(vertex.normal)`

### 2.2.2 [src/render/Mesh.cpp](../../../../src/render/Mesh.cpp) — `fromOBJ`

把 `vertex.color = {1.0f, 1.0f, 1.0f};` 替换为：

```cpp
if (!attrib.normals.empty() && index.normal_index >= 0) {
    vertex.normal = {
        attrib.normals[3 * index.normal_index + 0],
        attrib.normals[3 * index.normal_index + 1],
        attrib.normals[3 * index.normal_index + 2],
    };
} else {
    vertex.normal = {0.0f, 1.0f, 0.0f};
}
```

注意 viking_room.obj 实际包含法线数据，所以这个分支会命中上分支，视觉上光照会正确。

### 2.2.3 [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) — 最小修复

当前第 108 行：

```cpp
v.color = {1.0f, 1.0f, 1.0f};
```

改为：

```cpp
v.normal = {0.0f, 1.0f, 0.0f};   // Step 5 会换成真正读 NORMAL accessor
```

**不**在本步引入 NORMAL accessor 的完整读取 —— 保持 Step 5 的职责边界清晰。SheenChair 在 Step 2 之后看起来会**更黑**（假法线全朝 +Y，光只从一个方向打），这是预期的；本步不追求它好看。

### 2.2.4 `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`）

完整替换：

```glsl
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragNormalWS;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    // 仅刚体 + 均匀缩放时可行；非均匀缩放将来需 normalMatrix
    fragNormalWS = mat3(push.model) * inNormal;
    fragTexCoord = inTexCoord;
}
```

### 2.2.5 `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`）

完整替换：

```glsl
#version 450

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(texSampler, fragTexCoord);
    vec3 n = normalize(fragNormalWS);
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    vec3 rgb = tex.rgb * (0.25 + 0.75 * ndl);
    outColor = vec4(rgb, tex.a);
}
```

### 2.2.6 重新编译 shader

```powershell
cd C:\Project\vulkan_learn\shader
.\compile.bat
```
预期输出：`vert.spv` 和 `frag.spv` 文件时间戳被刷新，**无 glslc error/warning**。`compile.bat` 末尾的 `pause` 会要求按键；在脚本式运行时可忽略等待或手动回车。

CMake 会把 `shader/*.spv` 复制到 `build*/<Config>/shader/`，构建一次 C++ 即可同步；不需要手动复制。

---

## 2.3 验证

### 2.3.1 构建

```powershell
cmake --build build-debug --config Debug 2>&1 | Select-String "error " | Select-Object -First 30
cmake --build build       --config Release 2>&1 | Select-String "error " | Select-Object -First 30
```
两者应 0 error。若出现 "offsetof on non-standard-layout type" 之类警告 → 忽略（项目已有此类 warning）。

### 2.3.2 运行

```powershell
cd C:\Project\vulkan_learn\build\Release
.\VulkanLab.exe
```
人眼判定：

| 场景 | 预期 |
| --- | --- |
| Viking Room | **有明暗对比**的软光照（屋顶亮、背光面暗）；贴图 UV 正确不错位 |
| SheenChair | 仍贴着 viking_room 纹理（已知 Bug，Step 5 才修），但现在整体偏暗（假法线恒为 +Y） |
| 控制台 | 无 `VUID-*` 报错；可见旧的 `Vertices: ... Indices: ...` 输出 |

### 2.3.3 常见故障定位

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 全黑 | `inNormal` 被 pipeline 当作未写入 / shader 未重编 | 检查 `compile.bat` 是否重跑；确认 `build\Release\shader\` 下 spv 时间戳刷新 |
| 画面还是老样子（无光照差异） | spv 未更新 | 手动删 `build*\*\shader\*.spv` 后重新 build C++ 让 CMake 拷贝 |
| Validation error "vertex attribute format does not match" | attribute description 未同步 | 确认 Vertex.h 的 `offsetof(Vertex, normal)` 改对 |
| 黑色 + validation "Read from image without valid layout" | 无关 Step 2，是偶发既有问题；观察是否也在 Step 1 前出现 | 记录不修 |

---

## 2.4 回滚

```powershell
git reset --hard HEAD     # 丢弃未提交改动
# 或 commit 后：
git revert HEAD
```
回滚后 Texture 重构仍在，不影响 Step 1。

---

## 2.5 Commit

```powershell
git add src/render/Vertex.h src/render/Mesh.cpp src/render/GltfLoader.cpp `
        shader/shader.vert shader/shader.frag shader/vert.spv shader/frag.spv
git commit -m "refactor(vertex): replace color with normal; add soft lighting in frag

- Vertex layout: location 1 is now vec3 normal (was vec3 color).
- Mesh::fromOBJ reads attrib.normals when available; fallback +Y.
- GltfLoader: temporary +Y normal (real NORMAL parsing in Step 5).
- Shaders rewritten to pass normalWS and do N.L soft shading.
- Compiled SPIR-V updated via shader/compile.bat."
```

> 如果项目 `.gitignore` 不跟踪 `*.spv`，最后两个路径会被拒绝，忽略即可；CMake 已负责在构建时从源头复制。

---

## 2.6 完成后状态

- Vertex ABI：`pos(vec3) + normal(vec3) + texCoord(vec2)` = 32B
- Shader push constant 仍是 `mat4 model` = 64B（Step 3 再扩到 128B）
- Viking Room 视觉升级为带光照；SheenChair 视觉**暂时退化**（预期）
- 为 Step 3 (Material push factors) / Step 5 (GltfLoader 读 NORMAL) 扫清依赖
