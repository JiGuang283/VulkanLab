#include "GltfPreparer.h"

#include "MaterialTextureSlot.h"
#include "TangentGenerator.h"
#include "TextureData.h"
#include "assets/DerivedTextureCache.h"
#include "assets/DerivedTextureManifest.h"
#include "core/Log.h"
#include "diagnostics/SceneLoadStats.h"
#include "diagnostics/Profiling.h"

#include "tiny_gltf_v3.h"

#include <stb_image.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkr {

namespace {

void throwIfCancelled(const CancellationToken &token) {
    if (token.cancelled())
        throw std::runtime_error("scene load cancelled");
}

const tg3_str_int_pair *findAttr(const tg3_primitive &primitive,
                                 const char *name) {
    for (uint32_t i = 0; i < primitive.attributes_count; ++i) {
        if (tg3_str_equals_cstr(primitive.attributes[i].key, name))
            return &primitive.attributes[i];
    }
    return nullptr;
}

std::string toString(tg3_str value) {
    if (!value.data || value.len == 0)
        return {};
    return std::string(value.data, value.len);
}

bool isRemoteUri(const std::string &uri) {
    return uri.rfind("http://", 0) == 0 ||
           uri.rfind("https://", 0) == 0;
}

bool readFileBytes(const std::filesystem::path &path,
                   std::vector<uint8_t> &out) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char *>(out.data()), size);
    return file.good();
}

const uint8_t *accessorBase(const tg3_model *model, int accessorIndex) {
    const tg3_accessor &accessor = model->accessors[accessorIndex];
    const tg3_buffer_view &view =
        model->buffer_views[accessor.buffer_view];
    return model->buffers[view.buffer].data.data + view.byte_offset +
           accessor.byte_offset;
}

int32_t accessorStride(const tg3_model *model, int accessorIndex) {
    const tg3_accessor &accessor = model->accessors[accessorIndex];
    const tg3_buffer_view &view =
        model->buffer_views[accessor.buffer_view];
    return tg3_accessor_byte_stride(&accessor, &view);
}

VkSamplerAddressMode samplerAddressMode(int32_t mode) {
    switch (mode) {
    case TG3_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TG3_TEXTURE_WRAP_MIRRORED_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TG3_TEXTURE_WRAP_REPEAT:
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter samplerFilter(int32_t filter) {
    switch (filter) {
    case TG3_TEXTURE_FILTER_NEAREST:
    case TG3_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
    case TG3_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
        return VK_FILTER_NEAREST;
    default:
        return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode samplerMipmapMode(int32_t filter) {
    switch (filter) {
    case TG3_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
    case TG3_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    default:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

void applySampler(const tg3_model *model, const tg3_texture &texture,
                  PreparedTexture &prepared) {
    if (texture.sampler < 0 ||
        texture.sampler >= static_cast<int32_t>(model->samplers_count)) {
        return;
    }
    const tg3_sampler &sampler = model->samplers[texture.sampler];
    prepared.wrapU = samplerAddressMode(sampler.wrap_s);
    prepared.wrapV = samplerAddressMode(sampler.wrap_t);
    prepared.minFilter = samplerFilter(sampler.min_filter);
    prepared.magFilter = samplerFilter(sampler.mag_filter);
    prepared.mipmapMode = samplerMipmapMode(sampler.min_filter);
}

glm::mat4 nodeLocalMatrix(const tg3_node &node) {
    if (node.has_matrix)
        return glm::mat4(glm::make_mat4(node.matrix));
    const glm::vec3 translation{
        static_cast<float>(node.translation[0]),
        static_cast<float>(node.translation[1]),
        static_cast<float>(node.translation[2])};
    const glm::quat rotation{static_cast<float>(node.rotation[3]),
                             static_cast<float>(node.rotation[0]),
                             static_cast<float>(node.rotation[1]),
                             static_cast<float>(node.rotation[2])};
    const glm::vec3 scale{static_cast<float>(node.scale[0]),
                          static_cast<float>(node.scale[1]),
                          static_cast<float>(node.scale[2])};
    return glm::translate(glm::mat4(1.0f), translation) *
           glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

bool finiteVec3(const glm::vec3 &value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

float sanitizeNonNegativeFloat(double value, float fallback,
                               bool &corrected) {
    if (!std::isfinite(value)) {
        corrected = true;
        return fallback;
    }
    if (value < 0.0) {
        corrected = true;
        return 0.0f;
    }
    if (value > static_cast<double>(std::numeric_limits<float>::max())) {
        corrected = true;
        return std::numeric_limits<float>::max();
    }
    return static_cast<float>(value);
}

std::optional<SceneLight>
makeSceneLight(const tg3_light &source, uint32_t lightIndex,
               const tg3_node &node, int nodeIndex, const glm::mat4 &world,
               const std::string &path) {
    SceneLight result{};
    if (tg3_str_equals_cstr(source.type, "directional")) {
        result.type = LightType::Directional;
    } else if (tg3_str_equals_cstr(source.type, "point")) {
        result.type = LightType::Point;
    } else if (tg3_str_equals_cstr(source.type, "spot")) {
        result.type = LightType::Spot;
    } else {
        VKR_LOG_WARN("Gltf",
                     "{} light[{}] referenced by node[{}] has unknown type "
                     "'{}'; skipping",
                     path, lightIndex, nodeIndex, toString(source.type));
        return std::nullopt;
    }

    std::string lightName = toString(source.name);
    if (lightName.empty())
        lightName = "Light #" + std::to_string(lightIndex);
    const std::string nodeName = toString(node.name);
    result.debugName =
        nodeName.empty() || nodeName == lightName
            ? lightName + " [node " + std::to_string(nodeIndex) + "]"
            : lightName + " @ " + nodeName;

    bool correctedProperties = false;
    for (glm::length_t component = 0; component < 3; ++component) {
        result.color[component] =
            sanitizeNonNegativeFloat(source.color[component], 1.0f,
                                     correctedProperties);
    }
    result.intensity = sanitizeNonNegativeFloat(
        source.intensity, 1.0f, correctedProperties);

    if (result.type != LightType::Directional) {
        const glm::vec3 position = glm::vec3(world[3]);
        if (!finiteVec3(position)) {
            VKR_LOG_WARN("Gltf",
                         "{} light[{}] node[{}] has a non-finite world "
                         "position; skipping",
                         path, lightIndex, nodeIndex);
            return std::nullopt;
        }
        result.positionWS = position;
        result.range = sanitizeNonNegativeFloat(
            source.range, 0.0f, correctedProperties);
    } else {
        result.range = 0.0f;
    }

    if (result.type == LightType::Directional ||
        result.type == LightType::Spot) {
        const glm::vec3 emittedDirection =
            glm::mat3(world) * glm::vec3(0.0f, 0.0f, -1.0f);
        const float directionLengthSquared =
            glm::dot(emittedDirection, emittedDirection);
        if (!finiteVec3(emittedDirection) ||
            !std::isfinite(directionLengthSquared) ||
            directionLengthSquared <= 1.0e-8f) {
            VKR_LOG_WARN("Gltf",
                         "{} light[{}] node[{}] has an invalid world "
                         "direction; skipping",
                         path, lightIndex, nodeIndex);
            return std::nullopt;
        }
        const glm::vec3 normalizedDirection =
            emittedDirection / std::sqrt(directionLengthSquared);
        result.directionWS =
            result.type == LightType::Directional
                ? -normalizedDirection
                : normalizedDirection;
    }

    if (result.type == LightType::Spot) {
        constexpr double halfPi = 1.57079632679489661923;
        double inner = source.spot.inner_cone_angle;
        double outer = source.spot.outer_cone_angle;
        if (!std::isfinite(inner) || !std::isfinite(outer) || inner < 0.0 ||
            outer <= 0.0 || outer > halfPi || inner >= outer) {
            correctedProperties = true;
            inner = 0.0;
            outer = 0.78539816339744830962;
        }
        result.innerConeCos = static_cast<float>(std::cos(inner));
        result.outerConeCos = static_cast<float>(std::cos(outer));
    }

    if (correctedProperties) {
        VKR_LOG_WARN(
            "Gltf",
            "{} light[{}] node[{}] contains invalid properties; corrected "
            "to safe values",
            path, lightIndex, nodeIndex);
    }
    return result;
}

const tg3_extension *findExtension(const tg3_extras_ext &extensions,
                                   const char *name) {
    for (uint32_t i = 0; i < extensions.extensions_count; ++i) {
        if (tg3_str_equals_cstr(extensions.extensions[i].name, name))
            return &extensions.extensions[i];
    }
    return nullptr;
}

const tg3_value *objectValue(const tg3_value *value, const char *key) {
    if (!value || value->type != TG3_VALUE_OBJECT)
        return nullptr;
    for (uint32_t i = 0; i < value->object_count; ++i) {
        if (tg3_str_equals_cstr(value->object_data[i].key, key))
            return &value->object_data[i].value;
    }
    return nullptr;
}

bool numberValue(const tg3_value *value, float &out) {
    if (!value)
        return false;
    if (value->type == TG3_VALUE_REAL) {
        out = static_cast<float>(value->real_val);
        return true;
    }
    if (value->type == TG3_VALUE_INT) {
        out = static_cast<float>(value->int_val);
        return true;
    }
    return false;
}

bool integerValue(const tg3_value *value, int &out) {
    if (!value || value->type != TG3_VALUE_INT)
        return false;
    out = static_cast<int>(value->int_val);
    return true;
}

bool vec3Value(const tg3_value *value, glm::vec3 &out) {
    if (!value || value->type != TG3_VALUE_ARRAY || value->array_count < 3)
        return false;
    glm::vec3 result{};
    for (uint32_t i = 0; i < 3; ++i) {
        float component = 0.0f;
        if (!numberValue(&value->array_data[i], component))
            return false;
        result[static_cast<int>(i)] = component;
    }
    out = result;
    return true;
}

void parseMaterialExtensions(const tg3_material &material,
                             MaterialParams &params,
                             const std::string &path,
                             uint32_t materialIndex) {
    if (const tg3_extension *transmission = findExtension(
            material.ext, "KHR_materials_transmission")) {
        float factor = params.transmissionFactor;
        if (numberValue(
                objectValue(&transmission->value, "transmissionFactor"),
                factor)) {
            params.transmissionFactor = glm::clamp(factor, 0.0f, 1.0f);
        }
        const tg3_value *texture =
            objectValue(&transmission->value, "transmissionTexture");
        int textureIndex = -1;
        if (integerValue(objectValue(texture, "index"), textureIndex)) {
            VKR_LOG_WARN("Gltf",
                         "{} material[{}] uses transmissionTexture[{}], "
                         "which is not supported.",
                         path, materialIndex, textureIndex);
        }
    }

    if (const tg3_extension *emissive = findExtension(
            material.ext, "KHR_materials_emissive_strength")) {
        float strength = params.emissiveStrength;
        if (numberValue(
                objectValue(&emissive->value, "emissiveStrength"),
                strength)) {
            params.emissiveStrength = glm::max(strength, 0.0f);
        }
    }

    if (const tg3_extension *volume =
            findExtension(material.ext, "KHR_materials_volume")) {
        float thickness = params.thicknessFactor;
        if (numberValue(objectValue(&volume->value, "thicknessFactor"),
                        thickness)) {
            params.thicknessFactor = glm::max(thickness, 0.0f);
        }
        glm::vec3 attenuation = params.attenuationColor;
        if (vec3Value(objectValue(&volume->value, "attenuationColor"),
                      attenuation)) {
            params.attenuationColor = glm::clamp(attenuation, 0.0f, 1.0f);
        }
        float distance = params.attenuationDistance;
        if (numberValue(
                objectValue(&volume->value, "attenuationDistance"),
                distance)) {
            params.attenuationDistance = glm::max(distance, 0.0f);
        }
    }
}

Bounds computeBounds(const std::vector<Vertex> &vertices) {
    Bounds bounds{};
    if (vertices.empty())
        return bounds;
    glm::vec3 minValue{std::numeric_limits<float>::max()};
    glm::vec3 maxValue{std::numeric_limits<float>::lowest()};
    for (const Vertex &vertex : vertices) {
        minValue = glm::min(minValue, vertex.pos);
        maxValue = glm::max(maxValue, vertex.pos);
    }
    bounds.min = minValue;
    bounds.max = maxValue;
    bounds.center = (minValue + maxValue) * 0.5f;
    bounds.radius = glm::length(maxValue - bounds.center);
    bounds.valid = true;
    return bounds;
}

VkFormat formatForSlot(MaterialTextureSlot slot) {
    return slot == MaterialTextureSlot::BaseColor ||
                   slot == MaterialTextureSlot::Emissive
               ? VK_FORMAT_R8G8B8A8_SRGB
               : VK_FORMAT_R8G8B8A8_UNORM;
}

TextureSemantic semanticForSlot(MaterialTextureSlot slot) {
    if (slot == MaterialTextureSlot::BaseColor ||
        slot == MaterialTextureSlot::Emissive) {
        return TextureSemantic::SrgbColor;
    }
    if (slot == MaterialTextureSlot::Normal)
        return TextureSemantic::Normal;
    return TextureSemantic::LinearData;
}

DerivedMipmapWrap mipmapWrapFor(const PreparedTexture &texture) {
    if (texture.wrapU == VK_SAMPLER_ADDRESS_MODE_REPEAT &&
        texture.wrapV == VK_SAMPLER_ADDRESS_MODE_REPEAT) {
        return DerivedMipmapWrap::Repeat;
    }
    if (texture.wrapU == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
        texture.wrapV == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT) {
        return DerivedMipmapWrap::Reflect;
    }
    return DerivedMipmapWrap::Clamp;
}

uint64_t textureKey(int textureIndex, TextureSemantic semantic) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(textureIndex))
            << 32) |
           static_cast<uint32_t>(semantic);
}

} // namespace

PreparedSceneData GltfPreparer::prepare(
    const std::string &path, const Options &options,
    const CancellationToken &cancellation, SceneLoadProgress *progress) {
    VKL_PROFILE_ZONE("GltfPreparer::prepare");
    VKL_PROFILE_TEXT(path);
    throwIfCancelled(cancellation);

    tinygltf3::Model model;
    tinygltf3::ErrorStack errors;
    tg3_parse_options parseOptions;
    tg3_parse_options_init(&parseOptions);
    parseOptions.images_as_is = 1;

    tg3_error_code parseResult = TG3_OK;
    {
        VKL_PROFILE_ZONE("glTF Parse");
        ScopedLoadTimer timer(options.loadStats
                                  ? &options.loadStats->gltfParseMs
                                  : nullptr);
        parseResult = tinygltf3::parse_file(model, errors, path.c_str(),
                                            &parseOptions);
    }
    if (parseResult != TG3_OK || errors.has_error()) {
        std::string message = "GltfPreparer: failed to load '" + path + "'";
        for (uint32_t i = 0; i < errors.count(); ++i) {
            const tg3_error_entry *entry = errors.entry(i);
            if (entry && entry->severity == TG3_SEVERITY_ERROR &&
                entry->message) {
                message += "\n  " + std::string(entry->message);
            }
        }
        throw std::runtime_error(message);
    }

    throwIfCancelled(cancellation);
    const tg3_model *gltf = model.get();
    PreparedSceneData prepared;
    prepared.sourcePath = path;
    prepared.initialCamera = options.cameraOverride;
    if (options.loadStats)
        options.loadStats->gltfLightDefinitionCount = gltf->lights_count;

    if (progress) {
        progress->totalTextures = gltf->textures_count;
        uint64_t primitiveCount = 0;
        for (uint32_t i = 0; i < gltf->meshes_count; ++i)
            primitiveCount += gltf->meshes[i].primitives_count;
        progress->totalMeshes = primitiveCount;
    }

    const std::filesystem::path modelPath(path);
    const std::filesystem::path modelDirectory =
        modelPath.has_parent_path() ? modelPath.parent_path()
                                   : std::filesystem::path(".");

    DerivedTextureCache derivedCache(
        std::filesystem::u8path(options.derivedTextureCachePath), modelPath,
        options.projectId,
        options.sceneId, options.profileId, options.maxTextureSize,
        options.textureTranscodeTarget,
        options.requireDerivedTextures,
        options.loadStats ? &options.loadStats->resources : nullptr);

    std::unordered_map<int, std::shared_ptr<const PreparedImage>> imageCache;
    std::unordered_map<uint64_t, int32_t> textureCache;

    const auto decodeImage = [&](int imageIndex)
        -> std::shared_ptr<const PreparedImage> {
        const auto cached = imageCache.find(imageIndex);
        if (cached != imageCache.end())
            return cached->second;
        if (imageIndex < 0 || imageIndex >= static_cast<int>(gltf->images_count))
            return {};

        throwIfCancelled(cancellation);
        const tg3_image &image = gltf->images[imageIndex];
        const uint8_t *encoded = nullptr;
        size_t encodedSize = 0;
        std::vector<uint8_t> externalBytes;

        if (image.buffer_view >= 0 &&
            image.buffer_view < static_cast<int>(gltf->buffer_views_count)) {
            const tg3_buffer_view &view =
                gltf->buffer_views[image.buffer_view];
            const tg3_buffer &buffer = gltf->buffers[view.buffer];
            encoded = buffer.data.data + view.byte_offset;
            encodedSize = static_cast<size_t>(view.byte_length);
        } else if (image.image.data && image.image.count > 0) {
            encoded = image.image.data;
            encodedSize = static_cast<size_t>(image.image.count);
        } else {
            const std::string uri = toString(image.uri);
            if (uri.empty() || isRemoteUri(uri) || uri.rfind("data:", 0) == 0)
                return {};
            const std::filesystem::path imagePath =
                std::filesystem::path(uri).is_absolute()
                    ? std::filesystem::path(uri)
                    : modelDirectory / std::filesystem::path(uri);
            bool read = false;
            {
                ScopedLoadTimer timer(
                    options.loadStats
                        ? &options.loadStats->textureFileReadMs
                        : nullptr);
                read = readFileBytes(imagePath, externalBytes);
            }
            if (!read) {
                VKR_LOG_WARN("Gltf", "Could not read image '{}'; using fallback",
                             imagePath.string());
                return {};
            }
            encoded = externalBytes.data();
            encodedSize = externalBytes.size();
        }

        if (options.loadStats)
            options.loadStats->resources.encodedSourceBytes += encodedSize;
        if (progress)
            progress->processedBytes += encodedSize;

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *decoded = nullptr;
        {
            ScopedLoadTimer timer(options.loadStats
                                      ? &options.loadStats->textureDecodeMs
                                      : nullptr);
            decoded = stbi_load_from_memory(
                encoded, static_cast<int>(encodedSize), &width, &height,
                &channels, STBI_rgb_alpha);
        }
        if (!decoded) {
            VKR_LOG_WARN("Gltf", "Could not decode image[{}]; using fallback",
                         imageIndex);
            return {};
        }
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decodedOwner(
            decoded, &stbi_image_free);

        const uint64_t decodedBytes = static_cast<uint64_t>(width) * height * 4;
        if (options.loadStats) {
            ++options.loadStats->resources.textureDecodeCount;
            options.loadStats->resources.decodedRgbaBytes += decodedBytes;
        }

        uint32_t outputWidth = static_cast<uint32_t>(width);
        uint32_t outputHeight = static_cast<uint32_t>(height);
        const bool resize = limitedTextureExtent(
            outputWidth, outputHeight, options.maxTextureSize, outputWidth,
            outputHeight);

        auto result = std::make_shared<PreparedImage>();
        result->width = outputWidth;
        result->height = outputHeight;
        if (resize) {
            ScopedLoadTimer timer(
                options.loadStats
                    ? &options.loadStats->resources.textureResizeMs
                    : nullptr);
            result->pixels = resizeRgba8Bilinear(
                decodedOwner.get(), static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), outputWidth, outputHeight);
            if (options.loadStats)
                ++options.loadStats->resources.resizedTextureCount;
            VKR_LOG_INFO("Texture", "Prepared texture {}x{} -> {}x{} (limit {})",
                         width, height, outputWidth, outputHeight,
                         options.maxTextureSize);
        } else {
            result->pixels.assign(decodedOwner.get(),
                                  decodedOwner.get() + decodedBytes);
        }
        if (progress)
            progress->processedBytes += result->pixels.size();
        imageCache.emplace(imageIndex, result);
        return result;
    };

    const auto pickTexture = [&](int gltfTextureIndex,
                                 MaterialTextureSlot slot) -> int32_t {
        if (gltfTextureIndex < 0 ||
            gltfTextureIndex >= static_cast<int>(gltf->textures_count)) {
            return -1;
        }
        const tg3_texture &texture = gltf->textures[gltfTextureIndex];
        if (texture.source < 0 ||
            texture.source >= static_cast<int>(gltf->images_count)) {
            return -1;
        }
        const VkFormat format = formatForSlot(slot);
        const TextureSemantic semantic = semanticForSlot(slot);
        const uint64_t key = textureKey(gltfTextureIndex, semantic);
        const auto cached = textureCache.find(key);
        if (cached != textureCache.end())
            return cached->second;

        PreparedTexture preparedTexture;
        preparedTexture.debugName =
            "Image" + std::to_string(texture.source) + "/" +
            textureSemanticName(semantic);
        preparedTexture.format = format;
        applySampler(gltf, texture, preparedTexture);
        auto image = derivedCache.load(texture.source, semantic,
                                       mipmapWrapFor(preparedTexture));
        if (!image && !options.requireDerivedTextures)
            image = decodeImage(texture.source);
        if (!image && options.requireDerivedTextures) {
            throw std::runtime_error(
                "Cooked texture contract failed for glTF texture " +
                std::to_string(gltfTextureIndex));
        }
        if (!image)
            return -1;
        preparedTexture.image = image;
        if (image->format != VK_FORMAT_UNDEFINED)
            preparedTexture.format = image->format;
        const int32_t index = static_cast<int32_t>(prepared.textures.size());
        prepared.textures.push_back(std::move(preparedTexture));
        textureCache.emplace(key, index);
        if (progress) {
            ++progress->completedTextures;
            progress->totalTextures = std::max<uint64_t>(
                progress->totalTextures.load(), prepared.textures.size());
        }
        return index;
    };

    VKL_PROFILE_BEGIN(materialsProfile, "glTF Materials And Textures");
    prepared.materials.reserve(gltf->materials_count);
    for (uint32_t i = 0; i < gltf->materials_count; ++i) {
        throwIfCancelled(cancellation);
        const tg3_material &material = gltf->materials[i];
        PreparedMaterial result;
        {
            ScopedLoadTimer timer(options.loadStats
                                      ? &options.loadStats->materialSetupMs
                                      : nullptr);
            MaterialParams &params = result.params;
            params.debugName = toString(material.name);
            if (params.debugName.empty())
                params.debugName = "Material #" + std::to_string(i);
            const double *base =
                material.pbr_metallic_roughness.base_color_factor;
            params.baseColorFactor = {
                static_cast<float>(base[0]), static_cast<float>(base[1]),
                static_cast<float>(base[2]), static_cast<float>(base[3])};
            params.metallicFactor = static_cast<float>(
                material.pbr_metallic_roughness.metallic_factor);
            params.roughnessFactor = static_cast<float>(
                material.pbr_metallic_roughness.roughness_factor);
            params.emissiveFactor = {
                static_cast<float>(material.emissive_factor[0]),
                static_cast<float>(material.emissive_factor[1]),
                static_cast<float>(material.emissive_factor[2])};
            params.alphaCutoff = static_cast<float>(material.alpha_cutoff);
            if (tg3_str_equals_cstr(material.alpha_mode, "MASK"))
                params.alphaMode = AlphaMode::Mask;
            else if (tg3_str_equals_cstr(material.alpha_mode, "BLEND"))
                params.alphaMode = AlphaMode::Blend;
            params.doubleSided = material.double_sided != 0;
            if (material.normal_texture.index >= 0) {
                params.normalScale = glm::max(
                    static_cast<float>(material.normal_texture.scale), 0.0f);
            }
            params.occlusionStrength = glm::clamp(
                static_cast<float>(material.occlusion_texture.strength),
                0.0f, 1.0f);
            if (material.occlusion_texture.index >= 0) {
                const int coord = material.occlusion_texture.tex_coord;
                if (coord == 0 || coord == 1)
                    params.occlusionTexCoord = static_cast<uint32_t>(coord);
                else
                    VKR_LOG_WARN("Gltf",
                                 "{} material[{}] uses unsupported AO UV {}",
                                 path, i, coord);
            }
            parseMaterialExtensions(material, params, path, i);
        }

        result.textureIndices[indexOf(MaterialTextureSlot::BaseColor)] =
            pickTexture(
                material.pbr_metallic_roughness.base_color_texture.index,
                MaterialTextureSlot::BaseColor);
        result.textureIndices[indexOf(MaterialTextureSlot::Normal)] =
            pickTexture(material.normal_texture.index,
                        MaterialTextureSlot::Normal);
        result.textureIndices[indexOf(
            MaterialTextureSlot::MetallicRoughness)] = pickTexture(
            material.pbr_metallic_roughness.metallic_roughness_texture.index,
            MaterialTextureSlot::MetallicRoughness);
        result.textureIndices[indexOf(MaterialTextureSlot::Occlusion)] =
            pickTexture(material.occlusion_texture.index,
                        MaterialTextureSlot::Occlusion);
        result.textureIndices[indexOf(MaterialTextureSlot::Emissive)] =
            pickTexture(material.emissive_texture.index,
                        MaterialTextureSlot::Emissive);
        prepared.materials.push_back(std::move(result));
    }
    VKL_PROFILE_END(materialsProfile);

    struct PrimitiveReference {
        uint32_t meshIndex = 0;
        int32_t materialIndex = -1;
    };
    std::vector<std::vector<PrimitiveReference>> primitivesByMesh(
        gltf->meshes_count);

    VKL_PROFILE_BEGIN(meshProfile, "glTF Mesh Conversion");
    for (uint32_t meshIndex = 0; meshIndex < gltf->meshes_count;
         ++meshIndex) {
        const tg3_mesh &mesh = gltf->meshes[meshIndex];
        for (uint32_t primitiveIndex = 0;
             primitiveIndex < mesh.primitives_count; ++primitiveIndex) {
            throwIfCancelled(cancellation);
            const tg3_primitive &primitive = mesh.primitives[primitiveIndex];
            if (primitive.mode != -1 && primitive.mode != TG3_MODE_TRIANGLES)
                continue;
            const tg3_str_int_pair *positionAttribute =
                findAttr(primitive, "POSITION");
            if (!positionAttribute)
                continue;

            const auto cpuStart = std::chrono::steady_clock::now();
            const tg3_accessor &positionAccessor =
                gltf->accessors[positionAttribute->value];
            const uint64_t vertexCount = positionAccessor.count;
            const uint8_t *positionBase =
                accessorBase(gltf, positionAttribute->value);
            const int32_t positionStride =
                accessorStride(gltf, positionAttribute->value);

            const tg3_str_int_pair *normalAttribute =
                findAttr(primitive, "NORMAL");
            const tg3_str_int_pair *uvAttribute =
                findAttr(primitive, "TEXCOORD_0");
            const tg3_str_int_pair *uv1Attribute =
                findAttr(primitive, "TEXCOORD_1");
            const tg3_str_int_pair *tangentAttribute =
                findAttr(primitive, "TANGENT");
            const tg3_str_int_pair *colorAttribute =
                findAttr(primitive, "COLOR_0");

            const auto baseAndStride = [&](const tg3_str_int_pair *attribute) {
                return std::pair<const uint8_t *, int32_t>{
                    attribute ? accessorBase(gltf, attribute->value) : nullptr,
                    attribute ? accessorStride(gltf, attribute->value) : 0};
            };
            const auto [normalBase, normalStride] =
                baseAndStride(normalAttribute);
            const auto [uvBase, uvStride] = baseAndStride(uvAttribute);
            const auto [uv1Base, uv1Stride] = baseAndStride(uv1Attribute);
            const auto [tangentBase, tangentStride] =
                baseAndStride(tangentAttribute);

            const uint8_t *colorBase = nullptr;
            int32_t colorStride = 0;
            int32_t colorType = TG3_TYPE_VEC4;
            if (colorAttribute) {
                const tg3_accessor &accessor =
                    gltf->accessors[colorAttribute->value];
                if (accessor.component_type == TG3_COMPONENT_TYPE_FLOAT &&
                    (accessor.type == TG3_TYPE_VEC3 ||
                     accessor.type == TG3_TYPE_VEC4)) {
                    colorBase = accessorBase(gltf, colorAttribute->value);
                    colorStride = accessorStride(gltf, colorAttribute->value);
                    colorType = accessor.type;
                }
            }

            PreparedMesh result;
            result.debugName =
                "Mesh" + std::to_string(meshIndex) + "/Primitive" +
                std::to_string(primitiveIndex);
            result.vertices.resize(static_cast<size_t>(vertexCount));
            for (uint64_t vertexIndex = 0; vertexIndex < vertexCount;
                 ++vertexIndex) {
                Vertex &vertex = result.vertices[vertexIndex];
                const float *position = reinterpret_cast<const float *>(
                    positionBase + vertexIndex * positionStride);
                vertex.pos = {position[0], position[1], position[2]};
                if (normalBase) {
                    const float *normal = reinterpret_cast<const float *>(
                        normalBase + vertexIndex * normalStride);
                    vertex.normal = {normal[0], normal[1], normal[2]};
                } else {
                    vertex.normal = {0.0f, 1.0f, 0.0f};
                }
                if (uvBase) {
                    const float *uv = reinterpret_cast<const float *>(
                        uvBase + vertexIndex * uvStride);
                    vertex.texCoord = {uv[0], uv[1]};
                }
                if (uv1Base) {
                    const float *uv = reinterpret_cast<const float *>(
                        uv1Base + vertexIndex * uv1Stride);
                    vertex.texCoord1 = {uv[0], uv[1]};
                } else {
                    vertex.texCoord1 = vertex.texCoord;
                }
                if (tangentBase) {
                    const float *tangent = reinterpret_cast<const float *>(
                        tangentBase + vertexIndex * tangentStride);
                    vertex.tangent = {tangent[0], tangent[1], tangent[2],
                                      tangent[3]};
                }
                if (colorBase) {
                    const float *color = reinterpret_cast<const float *>(
                        colorBase + vertexIndex * colorStride);
                    vertex.color = {color[0], color[1], color[2],
                                    colorType == TG3_TYPE_VEC4 ? color[3]
                                                               : 1.0f};
                }
            }

            if (primitive.indices != TG3_INDEX_NONE) {
                const tg3_accessor &indexAccessor =
                    gltf->accessors[primitive.indices];
                const tg3_buffer_view &indexView =
                    gltf->buffer_views[indexAccessor.buffer_view];
                const uint8_t *indexBase =
                    gltf->buffers[indexView.buffer].data.data +
                    indexView.byte_offset + indexAccessor.byte_offset;
                const int32_t indexStride =
                    tg3_accessor_byte_stride(&indexAccessor, &indexView);
                result.indices.resize(static_cast<size_t>(indexAccessor.count));
                for (uint64_t i = 0; i < indexAccessor.count; ++i) {
                    const uint8_t *element = indexBase + i * indexStride;
                    switch (indexAccessor.component_type) {
                    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                        result.indices[i] = *element;
                        break;
                    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                        result.indices[i] =
                            *reinterpret_cast<const uint16_t *>(element);
                        break;
                    case TG3_COMPONENT_TYPE_UNSIGNED_INT:
                        result.indices[i] =
                            *reinterpret_cast<const uint32_t *>(element);
                        break;
                    default:
                        throw std::runtime_error(
                            "Unsupported glTF index component type");
                    }
                }
            } else {
                result.indices.resize(static_cast<size_t>(vertexCount));
                for (uint64_t i = 0; i < vertexCount; ++i)
                    result.indices[i] = static_cast<uint32_t>(i);
            }

            if (!tangentBase)
                generateTangents(result.vertices, result.indices);
            result.bounds = computeBounds(result.vertices);
            if (options.loadStats) {
                options.loadStats->meshCpuMs +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - cpuStart)
                        .count();
            }

            const uint32_t preparedMeshIndex =
                static_cast<uint32_t>(prepared.meshes.size());
            prepared.meshes.push_back(std::move(result));
            primitivesByMesh[meshIndex].push_back(
                {preparedMeshIndex, primitive.material});
            if (progress)
                ++progress->completedMeshes;
        }
    }
    VKL_PROFILE_END(meshProfile);

    VKL_PROFILE_BEGIN(hierarchyProfile, "glTF Hierarchy Finalize");
    const auto hierarchyStart = std::chrono::steady_clock::now();
    std::function<void(int, const glm::mat4 &)> walkNode =
        [&](int nodeIndex, const glm::mat4 &parent) {
            throwIfCancelled(cancellation);
            if (nodeIndex < 0 ||
                nodeIndex >= static_cast<int>(gltf->nodes_count)) {
                return;
            }
            const tg3_node &node = gltf->nodes[nodeIndex];
            const glm::mat4 world = parent * nodeLocalMatrix(node);
            if (node.mesh >= 0 &&
                node.mesh < static_cast<int>(primitivesByMesh.size())) {
                for (const PrimitiveReference &primitive :
                     primitivesByMesh[node.mesh]) {
                    prepared.objects.push_back(
                        {primitive.meshIndex, primitive.materialIndex, world});
                }
            }
            if (node.light >= 0) {
                if (node.light >= static_cast<int>(gltf->lights_count)) {
                    VKR_LOG_WARN(
                        "Gltf",
                        "{} node[{}] references invalid light index {}; "
                        "skipping",
                        path, nodeIndex, node.light);
                } else {
                    auto light = makeSceneLight(
                        gltf->lights[node.light],
                        static_cast<uint32_t>(node.light), node, nodeIndex,
                        world, path);
                    if (light)
                        prepared.lights.push_back(std::move(*light));
                }
            }
            for (uint32_t i = 0; i < node.children_count; ++i)
                walkNode(node.children[i], world);
        };

    int sceneIndex = -1;
    if (gltf->default_scene >= 0 &&
        gltf->default_scene < static_cast<int>(gltf->scenes_count)) {
        sceneIndex = gltf->default_scene;
    } else if (gltf->scenes_count > 0) {
        sceneIndex = 0;
    }

    if (sceneIndex >= 0) {
        const tg3_scene &scene = gltf->scenes[sceneIndex];
        const glm::mat4 yupToZup =
            glm::rotate(glm::mat4(1.0f), glm::radians(180.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(90.0f),
                        glm::vec3(1.0f, 0.0f, 0.0f));
        for (uint32_t i = 0; i < scene.nodes_count; ++i)
            walkNode(scene.nodes[i], yupToZup);
    } else {
        for (const auto &meshPrimitives : primitivesByMesh) {
            for (const PrimitiveReference &primitive : meshPrimitives) {
                prepared.objects.push_back(
                    {primitive.meshIndex, -1, glm::mat4(1.0f)});
            }
        }
    }

    uint64_t directionalLightCount = 0;
    uint64_t pointLightCount = 0;
    uint64_t spotLightCount = 0;
    for (const SceneLight &light : prepared.lights) {
        switch (light.type) {
        case LightType::Directional:
            ++directionalLightCount;
            break;
        case LightType::Point:
            ++pointLightCount;
            break;
        case LightType::Spot:
            ++spotLightCount;
            break;
        }
    }
    if (options.loadStats) {
        options.loadStats->hierarchyMs +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - hierarchyStart)
                .count();
        options.loadStats->lightInstanceCount = prepared.lights.size();
        options.loadStats->directionalLightCount =
            directionalLightCount;
        options.loadStats->pointLightCount = pointLightCount;
        options.loadStats->spotLightCount = spotLightCount;
    }
    if (gltf->lights_count > 0) {
        VKR_LOG_INFO(
            "Gltf",
            "Imported punctual lights from '{}': {} definitions, {} scene "
            "instances ({} directional, {} point, {} spot)",
            path, gltf->lights_count, prepared.lights.size(),
            directionalLightCount, pointLightCount, spotLightCount);
    }
    if (prepared.meshes.empty())
        throw std::runtime_error("No valid triangle primitives in '" + path +
                                 "'");
    if (options.loadStats) {
        uint64_t preparedBytes = 0;
        for (const auto &entry : imageCache)
            preparedBytes += entry.second ? entry.second->pixels.size() : 0;
        for (const PreparedMesh &mesh : prepared.meshes) {
            preparedBytes += mesh.vertices.size() * sizeof(Vertex);
            preparedBytes += mesh.indices.size() * sizeof(uint32_t);
        }
        for (const SceneLight &light : prepared.lights)
            preparedBytes += sizeof(SceneLight) + light.debugName.size();
        options.loadStats->preparedCpuBytes = preparedBytes;
    }
    throwIfCancelled(cancellation);
    VKL_PROFILE_END(hierarchyProfile);
    return prepared;
}

} // namespace vkr
