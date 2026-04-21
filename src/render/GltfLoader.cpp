#include "GltfLoader.h"
#include "Mesh.h"
#include "Vertex.h"
#include "core/Device.h"
#include "core/FrameSync.h"

// Only the declarations are needed here; the implementation lives in
// src/tiny_gltf.cpp (TINYGLTF3_IMPLEMENTATION + TINYGLTF3_ENABLE_FS).
#include "tiny_gltf_v3.h"

#include <cstdint>
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

// Pointer to the first element of an accessor's data.
static const uint8_t *accessorBase(const tg3_model *m, int accIdx) {
    const tg3_accessor    &acc = m->accessors[accIdx];
    const tg3_buffer_view &bv = m->buffer_views[acc.buffer_view];
    return m->buffers[bv.buffer].data.data + bv.byte_offset + acc.byte_offset;
}

// Byte stride between successive elements (accounts for interleaved layouts).
static int32_t accessorStride(const tg3_model *m, int accIdx) {
    const tg3_accessor    &acc = m->accessors[accIdx];
    const tg3_buffer_view &bv = m->buffer_views[acc.buffer_view];
    return tg3_accessor_byte_stride(&acc, &bv);
}

// ---- GltfLoader::load ------------------------------------------------------

std::vector<std::unique_ptr<Mesh>> GltfLoader::load(const std::string &path,
                                                    Device            &device,
                                                    FrameSync &frameSync) {
    tinygltf3::Model      model;
    tinygltf3::ErrorStack errors;

    tg3_parse_options opts;
    tg3_parse_options_init(&opts);
    opts.images_as_is = 1; // skip image decode — we only need geometry

    tg3_error_code rc =
        tinygltf3::parse_file(model, errors, path.c_str(), &opts);

    if (rc != TG3_OK || errors.has_error()) {
        std::string msg = "GltfLoader: failed to load '" + path + "'";
        for (uint32_t i = 0; i < errors.count(); ++i) {
            const tg3_error_entry *e = errors.entry(i);
            if (e && e->severity == TG3_SEVERITY_ERROR && e->message)
                msg += "\n  " + std::string(e->message);
        }
        throw std::runtime_error(msg);
    }

    const tg3_model                   *m = model.get();
    std::vector<std::unique_ptr<Mesh>> result;

    for (uint32_t mi = 0; mi < m->meshes_count; ++mi) {
        const tg3_mesh &mesh = m->meshes[mi];

        for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const tg3_primitive &prim = mesh.primitives[pi];

            // Skip anything that is not a triangle list
            // (mode == -1 means the default, which is TRIANGLES)
            if (prim.mode != -1 && prim.mode != TG3_MODE_TRIANGLES)
                continue;

            // POSITION is mandatory
            const tg3_str_int_pair *posAttr = findAttr(prim, "POSITION");
            if (!posAttr)
                continue;

            const tg3_accessor &posAcc = m->accessors[posAttr->value];
            uint64_t            vertCount = posAcc.count;
            const uint8_t      *posBase = accessorBase(m, posAttr->value);
            int32_t             posStride = accessorStride(m, posAttr->value);

            // TEXCOORD_0 is optional
            const tg3_str_int_pair *uvAttr = findAttr(prim, "TEXCOORD_0");
            const uint8_t          *uvBase = nullptr;
            int32_t                 uvStride = 0;
            if (uvAttr) {
                uvBase = accessorBase(m, uvAttr->value);
                uvStride = accessorStride(m, uvAttr->value);
            }

            // Build vertices
            std::vector<Vertex> verts(static_cast<size_t>(vertCount));
            for (uint64_t i = 0; i < vertCount; ++i) {
                Vertex      &v = verts[i];
                const float *p = reinterpret_cast<const float *>(
                    posBase + static_cast<size_t>(i) * posStride);
                v.pos = {p[0], p[1], p[2]};
                v.normal = {0.0f, 1.0f, 0.0f}; // Step 5: read NORMAL accessor
                if (uvBase) {
                    const float *uv = reinterpret_cast<const float *>(
                        uvBase + static_cast<size_t>(i) * uvStride);
                    v.texCoord = {uv[0], uv[1]};
                } else {
                    v.texCoord = {0.0f, 0.0f};
                }
            }

            // Build indices
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
                // No index buffer — generate sequential indices
                indices.resize(static_cast<size_t>(vertCount));
                for (uint64_t i = 0; i < vertCount; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }

            result.push_back(std::make_unique<Mesh>(
                device, frameSync, verts.data(), verts.size() * sizeof(Vertex),
                indices.data(), static_cast<uint32_t>(indices.size())));
        }
    }

    if (result.empty())
        throw std::runtime_error(
            "GltfLoader: no valid triangle primitives found in '" + path + "'");

    return result;
}

} // namespace vkr
