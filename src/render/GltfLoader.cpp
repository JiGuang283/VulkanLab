#include "GltfLoader.h"
#include "Material.h"
#include "Mesh.h"
#include "Texture.h"
#include "Vertex.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/PipelineConfig.h"

// Only the declarations are needed here; the implementation lives in
// src/tiny_gltf.cpp (TINYGLTF3_IMPLEMENTATION + TINYGLTF3_ENABLE_FS).
#include "tiny_gltf_v3.h"

#include <stb_image.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace vkr {

// ---- helpers ---------------------------------------------------------------

static const tg3_str_int_pair *findAttr(const tg3_primitive &prim,
                                        const char          *name) {
    for (uint32_t i = 0; i < prim.attributes_count; ++i)
        if (tg3_str_equals_cstr(prim.attributes[i].key, name))
            return &prim.attributes[i];
    return nullptr;
}

static const uint8_t *accessorBase(const tg3_model *m, int accIdx) {
    const tg3_accessor    &acc = m->accessors[accIdx];
    const tg3_buffer_view &bv = m->buffer_views[acc.buffer_view];
    return m->buffers[bv.buffer].data.data + bv.byte_offset + acc.byte_offset;
}

static int32_t accessorStride(const tg3_model *m, int accIdx) {
    const tg3_accessor    &acc = m->accessors[accIdx];
    const tg3_buffer_view &bv = m->buffer_views[acc.buffer_view];
    return tg3_accessor_byte_stride(&acc, &bv);
}

static std::shared_ptr<Texture> makeWhite1x1(Device &d, FrameSync &fs) {
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    TextureCreateInfo    ci;
    ci.pixels = kWhite;
    ci.width = 1;
    ci.height = 1;
    ci.generateMipmaps = false;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    return std::make_shared<Texture>(d, fs, ci);
}

// ---- GltfLoader::load ------------------------------------------------------

GltfAsset GltfLoader::load(const std::string    &path,
                           Device               &device,
                           FrameSync            &frameSync,
                           Renderer             &renderer,
                           const PipelineConfig &baseConfig,
                           const Options        &opts) {
    // 1. Parse
    tinygltf3::Model      model;
    tinygltf3::ErrorStack errors;

    tg3_parse_options popts;
    tg3_parse_options_init(&popts);
    popts.images_as_is = 1; // we decode images ourselves below

    tg3_error_code rc =
        tinygltf3::parse_file(model, errors, path.c_str(), &popts);
    if (rc != TG3_OK || errors.has_error()) {
        std::string msg = "GltfLoader: failed to load '" + path + "'";
        for (uint32_t i = 0; i < errors.count(); ++i) {
            const tg3_error_entry *e = errors.entry(i);
            if (e && e->severity == TG3_SEVERITY_ERROR && e->message)
                msg += "\n  " + std::string(e->message);
        }
        throw std::runtime_error(msg);
    }

    const tg3_model *m = model.get();
    GltfAsset        asset;

    // 2. Fallback white 1x1
    auto whiteTex =
        opts.fallbackWhite ? opts.fallbackWhite : makeWhite1x1(device, frameSync);

    // 3. Decode images -> intermediate textures (manual decode via stb_image,
    // since tg3 does not invoke its declared image callbacks).
    std::vector<std::shared_ptr<Texture>> imageTextures;
    imageTextures.reserve(m->images_count);
    for (uint32_t i = 0; i < m->images_count; ++i) {
        const tg3_image &img = m->images[i];
        const uint8_t   *encoded = nullptr;
        size_t           encodedSize = 0;
        if (img.buffer_view >= 0 &&
            img.buffer_view < (int)m->buffer_views_count) {
            const tg3_buffer_view &bv = m->buffer_views[img.buffer_view];
            const tg3_buffer      &buf = m->buffers[bv.buffer];
            encoded = buf.data.data + bv.byte_offset;
            encodedSize = static_cast<size_t>(bv.byte_length);
        } else if (img.image.data && img.image.count > 0) {
            encoded = img.image.data;
            encodedSize = static_cast<size_t>(img.image.count);
        }

        int      w = 0, h = 0, c = 0;
        stbi_uc *pixels =
            encoded ? stbi_load_from_memory(encoded,
                                            static_cast<int>(encodedSize), &w,
                                            &h, &c, STBI_rgb_alpha)
                    : nullptr;
        if (!pixels) {
            imageTextures.push_back(whiteTex);
            std::fprintf(stderr,
                         "[GltfLoader] image[%u] decode failed, using white.\n",
                         i);
            continue;
        }
        TextureCreateInfo ci;
        ci.pixels = pixels;
        ci.width = static_cast<uint32_t>(w);
        ci.height = static_cast<uint32_t>(h);
        ci.generateMipmaps = true;
        ci.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageTextures.push_back(
            std::make_shared<Texture>(device, frameSync, ci));
        stbi_image_free(pixels);
    }

    // 4. textures: v1 maps each glTF texture to its image (sampler ignored)
    asset.textures.reserve(m->textures_count);
    for (uint32_t i = 0; i < m->textures_count; ++i) {
        const tg3_texture &t = m->textures[i];
        if (t.source >= 0 && t.source < (int)imageTextures.size())
            asset.textures.push_back(imageTextures[t.source]);
        else
            asset.textures.push_back(whiteTex);
    }

    // 5. materials
    auto fallbackMat = std::make_shared<Material>(
        device, renderer, MaterialParams{whiteTex}, baseConfig);

    asset.materials.reserve(m->materials_count + 1);
    for (uint32_t i = 0; i < m->materials_count; ++i) {
        const tg3_material &gm = m->materials[i];
        MaterialParams      p;
        int bc = gm.pbr_metallic_roughness.base_color_texture.index;
        p.baseColor = (bc >= 0 && bc < (int)asset.textures.size())
                          ? asset.textures[bc]
                          : whiteTex;
        const double *bcf = gm.pbr_metallic_roughness.base_color_factor;
        p.baseColorFactor = {(float)bcf[0], (float)bcf[1], (float)bcf[2],
                             (float)bcf[3]};
        p.metallicFactor = (float)gm.pbr_metallic_roughness.metallic_factor;
        p.roughnessFactor = (float)gm.pbr_metallic_roughness.roughness_factor;
        const double *ef = gm.emissive_factor;
        p.emissiveFactor = {(float)ef[0], (float)ef[1], (float)ef[2]};
        p.alphaCutoff = (float)gm.alpha_cutoff;
        p.doubleSided = gm.double_sided != 0;
        asset.materials.push_back(std::make_shared<Material>(
            device, renderer, std::move(p), baseConfig));
    }
    asset.materials.push_back(fallbackMat);
    const size_t fallbackMatIdx = asset.materials.size() - 1;

    // 6. primitives -> Mesh + (mesh,materialIdx) registry
    struct PrimEntry {
        std::shared_ptr<Mesh> mesh;
        int                   materialIdx;
    };
    std::vector<std::vector<PrimEntry>> primsByMesh(m->meshes_count);

    for (uint32_t mi = 0; mi < m->meshes_count; ++mi) {
        const tg3_mesh &mesh = m->meshes[mi];
        for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const tg3_primitive &prim = mesh.primitives[pi];
            if (prim.mode != -1 && prim.mode != TG3_MODE_TRIANGLES)
                continue;

            const tg3_str_int_pair *posAttr = findAttr(prim, "POSITION");
            if (!posAttr)
                continue;

            const tg3_accessor &posAcc = m->accessors[posAttr->value];
            uint64_t            vertCount = posAcc.count;
            const uint8_t      *posBase = accessorBase(m, posAttr->value);
            int32_t             posStride = accessorStride(m, posAttr->value);

            const tg3_str_int_pair *nAttr = findAttr(prim, "NORMAL");
            const uint8_t          *nBase = nullptr;
            int32_t                 nStride = 0;
            if (nAttr) {
                nBase = accessorBase(m, nAttr->value);
                nStride = accessorStride(m, nAttr->value);
            }

            const tg3_str_int_pair *uvAttr = findAttr(prim, "TEXCOORD_0");
            const uint8_t          *uvBase = nullptr;
            int32_t                 uvStride = 0;
            if (uvAttr) {
                uvBase = accessorBase(m, uvAttr->value);
                uvStride = accessorStride(m, uvAttr->value);
            }

            std::vector<Vertex> verts(static_cast<size_t>(vertCount));
            for (uint64_t i = 0; i < vertCount; ++i) {
                Vertex      &v = verts[i];
                const float *p = reinterpret_cast<const float *>(
                    posBase + static_cast<size_t>(i) * posStride);
                v.pos = {p[0], p[1], p[2]};

                if (nBase) {
                    const float *n = reinterpret_cast<const float *>(
                        nBase + static_cast<size_t>(i) * nStride);
                    v.normal = {n[0], n[1], n[2]};
                } else {
                    v.normal = {0.0f, 1.0f, 0.0f};
                }

                if (uvBase) {
                    const float *uv = reinterpret_cast<const float *>(
                        uvBase + static_cast<size_t>(i) * uvStride);
                    v.texCoord = {uv[0], uv[1]};
                } else {
                    v.texCoord = {0.0f, 0.0f};
                }
            }

            std::vector<uint32_t> indices;
            if (prim.indices != TG3_INDEX_NONE) {
                const tg3_accessor    &idxAcc = m->accessors[prim.indices];
                const tg3_buffer_view &idxBv =
                    m->buffer_views[idxAcc.buffer_view];
                const uint8_t *idxBase = m->buffers[idxBv.buffer].data.data +
                                         idxBv.byte_offset + idxAcc.byte_offset;
                int32_t idxStride = tg3_accessor_byte_stride(&idxAcc, &idxBv);
                indices.resize(static_cast<size_t>(idxAcc.count));
                for (uint64_t i = 0; i < idxAcc.count; ++i) {
                    const uint8_t *elem =
                        idxBase + static_cast<size_t>(i) * idxStride;
                    switch (idxAcc.component_type) {
                    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                        indices[i] = *elem;
                        break;
                    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                        indices[i] = *reinterpret_cast<const uint16_t *>(elem);
                        break;
                    case TG3_COMPONENT_TYPE_UNSIGNED_INT:
                        indices[i] = *reinterpret_cast<const uint32_t *>(elem);
                        break;
                    default:
                        throw std::runtime_error(
                            "GltfLoader: unsupported index component type");
                    }
                }
            } else {
                indices.resize(static_cast<size_t>(vertCount));
                for (uint64_t i = 0; i < vertCount; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }

            auto mesh = std::make_shared<Mesh>(
                device, frameSync, verts.data(), verts.size() * sizeof(Vertex),
                indices.data(), static_cast<uint32_t>(indices.size()));
            asset.meshes.push_back(mesh);
            primsByMesh[mi].push_back({mesh, prim.material});
        }
    }

    // 7. v1: identity transform per primitive (Step 6 replaces with node walk)
    for (auto &list : primsByMesh) {
        for (auto &pe : list) {
            auto mat = (pe.materialIdx >= 0 &&
                        pe.materialIdx < (int)m->materials_count)
                           ? asset.materials[pe.materialIdx]
                           : asset.materials[fallbackMatIdx];
            asset.objects.push_back({pe.mesh, mat, glm::mat4(1.0f)});
        }
    }

    if (asset.meshes.empty())
        throw std::runtime_error(
            "GltfLoader: no valid triangle primitives found in '" + path + "'");

    return asset;
}

} // namespace vkr
