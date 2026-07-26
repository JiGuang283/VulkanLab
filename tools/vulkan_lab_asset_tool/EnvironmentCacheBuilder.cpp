#include "EnvironmentCacheBuilder.h"

#include "assets/ContentHash.h"
#include "assets/DerivedEnvironmentManifest.h"
#include "assets/DerivedTextureManifest.h"

#include <ktx.h>
#include <stb_image.h>
#include <vulkan/vulkan.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace vkr::assettool {
namespace {

constexpr const char *kAlgorithmVersion = "ibl-cpu-v1";

struct HdrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<glm::vec3> pixels;
};

struct BakedImage {
    EnvironmentMapKind kind = EnvironmentMapKind::Radiance;
    uint32_t vkFormat = VK_FORMAT_UNDEFINED;
    std::string formatName;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t faces = 1;
    uint32_t channels = 4;
    std::vector<std::vector<uint16_t>> subresources;
};

void checkCancelled(const EnvironmentCacheBuildOptions &options) {
    if (options.cancelRequested &&
        options.cancelRequested->load(std::memory_order_relaxed)) {
        throw std::runtime_error("environment cache build cancelled");
    }
}

uint32_t workerCount(uint32_t requested) {
    if (requested != 0)
        return requested;
    return std::max(1u, std::thread::hardware_concurrency());
}

void parallelRows(uint32_t rows, uint32_t workers,
                  const std::function<void(uint32_t)> &function) {
    std::atomic<uint32_t> next{0};
    std::vector<std::thread> threads;
    const uint32_t count = std::min(rows, std::max(1u, workers));
    threads.reserve(count);
    for (uint32_t worker = 0; worker < count; ++worker) {
        threads.emplace_back([&] {
            for (;;) {
                const uint32_t row =
                    next.fetch_add(1, std::memory_order_relaxed);
                if (row >= rows)
                    break;
                function(row);
            }
        });
    }
    for (std::thread &thread : threads)
        thread.join();
}

uint32_t fullMipCount(uint32_t size) {
    uint32_t levels = 1;
    while (size > 1) {
        size >>= 1;
        ++levels;
    }
    return levels;
}

BakedImage environmentImageDescriptor(EnvironmentMapKind kind,
                                      uint32_t size) {
    BakedImage image;
    image.kind = kind;
    image.width = size;
    image.height = size;
    if (kind == EnvironmentMapKind::BrdfLut) {
        image.vkFormat = VK_FORMAT_R16G16_SFLOAT;
        image.formatName = "rg16f";
        image.faces = 1;
        image.channels = 2;
    } else {
        image.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        image.formatName = "rgba16f";
        image.faces = 6;
        image.channels = 4;
    }
    if (kind == EnvironmentMapKind::Radiance ||
        kind == EnvironmentMapKind::PrefilteredSpecular) {
        image.mipLevels = fullMipCount(size);
    }
    return image;
}

HdrImage loadHdr(const std::filesystem::path &path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    float *decoded =
        stbi_loadf(path.string().c_str(), &width, &height, &channels, 3);
    if (!decoded)
        throw std::runtime_error("could not decode HDR environment: " +
                                 path.string());
    std::unique_ptr<float, decltype(&stbi_image_free)> owner(
        decoded, &stbi_image_free);
    if (width <= 0 || height <= 0 || width != height * 2) {
        throw std::runtime_error(
            "HDR environment must use a 2:1 equirectangular layout");
    }
    HdrImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels.resize(static_cast<size_t>(width) * height);
    for (size_t index = 0; index < image.pixels.size(); ++index) {
        glm::vec3 value(decoded[index * 3 + 0], decoded[index * 3 + 1],
                        decoded[index * 3 + 2]);
        for (int component = 0; component < 3; ++component) {
            if (!std::isfinite(value[component]))
                value[component] = 0.0f;
        }
        image.pixels[index] = glm::max(value, glm::vec3(0.0f));
    }
    return image;
}

glm::vec3 sampleEquirectangular(const HdrImage &image,
                                const glm::vec3 &worldDirection) {
    const glm::vec3 direction = glm::normalize(worldDirection);
    // Source HDRs use Y-up. Runtime world space is Z-up.
    const glm::vec3 sourceDirection(direction.x, direction.z, -direction.y);
    float u = std::atan2(sourceDirection.z, sourceDirection.x) /
                  glm::two_pi<float>() +
              0.5f;
    u -= std::floor(u);
    const float v =
        std::acos(std::clamp(sourceDirection.y, -1.0f, 1.0f)) /
        glm::pi<float>();
    const float x = u * static_cast<float>(image.width) - 0.5f;
    const float y =
        std::clamp(v * static_cast<float>(image.height) - 0.5f, 0.0f,
                   static_cast<float>(image.height - 1));
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t y0 = static_cast<int32_t>(std::floor(y));
    const int32_t x1 = x0 + 1;
    const int32_t y1 =
        std::min(y0 + 1, static_cast<int32_t>(image.height - 1));
    const auto wrapX = [&](int32_t value) {
        value %= static_cast<int32_t>(image.width);
        return value < 0 ? value + static_cast<int32_t>(image.width) : value;
    };
    const auto pixel = [&](int32_t px, int32_t py) {
        return image.pixels[static_cast<size_t>(py) * image.width +
                            static_cast<uint32_t>(wrapX(px))];
    };
    const float tx = x - std::floor(x);
    const float ty = y - std::floor(y);
    return glm::mix(glm::mix(pixel(x0, y0), pixel(x1, y0), tx),
                    glm::mix(pixel(x0, y1), pixel(x1, y1), tx), ty);
}

void orthonormalBasis(const glm::vec3 &normal, glm::vec3 &tangent,
                      glm::vec3 &bitangent) {
    const glm::vec3 helper =
        std::abs(normal.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                   : glm::vec3(0.0f, 1.0f, 0.0f);
    tangent = glm::normalize(glm::cross(helper, normal));
    bitangent = glm::cross(normal, tangent);
}

glm::vec3 localToWorld(const glm::vec3 &local, const glm::vec3 &normal) {
    glm::vec3 tangent;
    glm::vec3 bitangent;
    orthonormalBasis(normal, tangent, bitangent);
    return glm::normalize(tangent * local.x + bitangent * local.y +
                          normal * local.z);
}

glm::vec3 cosineHemisphere(const glm::vec2 &xi) {
    const float phi = glm::two_pi<float>() * xi.x;
    const float radius = std::sqrt(xi.y);
    return {radius * std::cos(phi), radius * std::sin(phi),
            std::sqrt(std::max(0.0f, 1.0f - xi.y))};
}

glm::vec3 importanceSampleGgx(const glm::vec2 &xi, float roughness,
                              const glm::vec3 &normal) {
    const float alpha = roughness * roughness;
    const float phi = glm::two_pi<float>() * xi.x;
    const float cosTheta =
        std::sqrt((1.0f - xi.y) /
                  (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta =
        std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return localToWorld(
        {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta},
        normal);
}

float geometrySchlickGgx(float nDotV, float roughness) {
    const float k = (roughness * roughness) * 0.5f;
    return nDotV / (nDotV * (1.0f - k) + k);
}

float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlickGgx(nDotV, roughness) *
           geometrySchlickGgx(nDotL, roughness);
}

std::vector<uint16_t> packHalf(const std::vector<glm::vec4> &pixels,
                               uint32_t channels) {
    std::vector<uint16_t> result;
    result.resize(pixels.size() * channels);
    for (size_t index = 0; index < pixels.size(); ++index) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            float value = pixels[index][channel];
            if (!std::isfinite(value))
                value = 0.0f;
            value = std::clamp(value, -65504.0f, 65504.0f);
            result[index * channels + channel] =
                glm::packHalf1x16(value);
        }
    }
    return result;
}

struct CubeCoordinate {
    uint32_t face = 0;
    float u = 0.0f;
    float v = 0.0f;
};

CubeCoordinate cubeCoordinate(const glm::vec3 &direction) {
    const glm::vec3 absolute = glm::abs(direction);
    CubeCoordinate coordinate;
    if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
        if (direction.x >= 0.0f) {
            coordinate.face = 0;
            coordinate.u = -direction.z / absolute.x;
            coordinate.v = -direction.y / absolute.x;
        } else {
            coordinate.face = 1;
            coordinate.u = direction.z / absolute.x;
            coordinate.v = -direction.y / absolute.x;
        }
    } else if (absolute.y >= absolute.z) {
        if (direction.y >= 0.0f) {
            coordinate.face = 2;
            coordinate.u = direction.x / absolute.y;
            coordinate.v = direction.z / absolute.y;
        } else {
            coordinate.face = 3;
            coordinate.u = direction.x / absolute.y;
            coordinate.v = -direction.z / absolute.y;
        }
    } else if (direction.z >= 0.0f) {
        coordinate.face = 4;
        coordinate.u = direction.x / absolute.z;
        coordinate.v = -direction.y / absolute.z;
    } else {
        coordinate.face = 5;
        coordinate.u = -direction.x / absolute.z;
        coordinate.v = -direction.y / absolute.z;
    }
    return coordinate;
}

glm::vec4 fetchCubeTexel(const std::vector<glm::vec4> *faces,
                         uint32_t size, uint32_t face, int32_t x,
                         int32_t y) {
    if (x >= 0 && y >= 0 && x < static_cast<int32_t>(size) &&
        y < static_cast<int32_t>(size)) {
        return faces[face][static_cast<size_t>(y) * size +
                           static_cast<uint32_t>(x)];
    }

    const float u =
        2.0f * (static_cast<float>(x) + 0.5f) /
            static_cast<float>(size) -
        1.0f;
    const float v =
        2.0f * (static_cast<float>(y) + 0.5f) /
            static_cast<float>(size) -
        1.0f;
    const CubeCoordinate remapped =
        cubeCoordinate(environmentCubeDirection(face, u, v));
    const int32_t remappedX = std::clamp(
        static_cast<int32_t>(
            std::floor((remapped.u * 0.5f + 0.5f) * size)),
        0, static_cast<int32_t>(size - 1));
    const int32_t remappedY = std::clamp(
        static_cast<int32_t>(
            std::floor((remapped.v * 0.5f + 0.5f) * size)),
        0, static_cast<int32_t>(size - 1));
    return faces[remapped.face]
                [static_cast<size_t>(remappedY) * size +
                 static_cast<uint32_t>(remappedX)];
}

float sinc(float value) {
    if (std::abs(value) < 1.0e-6f)
        return 1.0f;
    const float radians = glm::pi<float>() * value;
    return std::sin(radians) / radians;
}

float lanczos4(float value) {
    constexpr float radius = 4.0f;
    value = std::abs(value);
    return value < radius ? sinc(value) * sinc(value / radius) : 0.0f;
}

glm::vec4 sampleCubeLanczos4(const std::vector<glm::vec4> *faces,
                             uint32_t sourceSize, uint32_t face,
                             float sourceX, float sourceY, float scale) {
    constexpr float filterRadius = 4.0f;
    const float radius = filterRadius * scale;
    const int32_t firstX =
        static_cast<int32_t>(std::ceil(sourceX - radius));
    const int32_t lastX =
        static_cast<int32_t>(std::floor(sourceX + radius));
    const int32_t firstY =
        static_cast<int32_t>(std::ceil(sourceY - radius));
    const int32_t lastY =
        static_cast<int32_t>(std::floor(sourceY + radius));
    glm::vec4 sum(0.0f);
    float totalWeight = 0.0f;
    for (int32_t y = firstY; y <= lastY; ++y) {
        const float weightY =
            lanczos4((static_cast<float>(y) - sourceY) / scale);
        if (weightY == 0.0f)
            continue;
        for (int32_t x = firstX; x <= lastX; ++x) {
            const float weightX =
                lanczos4((static_cast<float>(x) - sourceX) / scale);
            const float weight = weightX * weightY;
            if (weight == 0.0f)
                continue;
            sum += fetchCubeTexel(faces, sourceSize, face, x, y) * weight;
            totalWeight += weight;
        }
    }
    const glm::vec4 filtered =
        totalWeight != 0.0f
            ? sum / totalWeight
            : fetchCubeTexel(
                  faces, sourceSize, face,
                  static_cast<int32_t>(std::round(sourceX)),
                  static_cast<int32_t>(std::round(sourceY)));
    return glm::vec4(glm::max(glm::vec3(filtered), glm::vec3(0.0f)),
                     1.0f);
}

BakedImage bakeRadiance(const HdrImage &source, uint32_t size,
                        uint32_t workers,
                        const EnvironmentCacheBuildOptions &options) {
    BakedImage image;
    image.kind = EnvironmentMapKind::Radiance;
    image.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    image.formatName = "rgba16f";
    image.width = size;
    image.height = size;
    image.mipLevels = fullMipCount(size);
    image.faces = 6;
    image.channels = 4;
    image.subresources.resize(
        static_cast<size_t>(image.mipLevels) * image.faces);

    std::vector<std::vector<glm::vec4>> levels(image.mipLevels * 6);
    for (uint32_t face = 0; face < 6; ++face)
        levels[face].resize(static_cast<size_t>(size) * size);
    parallelRows(size * 6, workers, [&](uint32_t combinedRow) {
        const uint32_t face = combinedRow / size;
        const uint32_t y = combinedRow % size;
        std::vector<glm::vec4> &pixels = levels[face];
        for (uint32_t x = 0; x < size; ++x) {
            const float u =
                (2.0f * (static_cast<float>(x) + 0.5f) / size) - 1.0f;
            const float v =
                (2.0f * (static_cast<float>(y) + 0.5f) / size) - 1.0f;
            pixels[static_cast<size_t>(y) * size + x] =
                glm::vec4(sampleEquirectangular(
                              source,
                              environmentCubeDirection(face, u, v)),
                          1.0f);
        }
    });
    checkCancelled(options);

    uint32_t previousSize = size;
    for (uint32_t level = 1; level < image.mipLevels; ++level) {
        const uint32_t levelSize = std::max(1u, previousSize / 2);
        const float scale =
            static_cast<float>(previousSize) /
            static_cast<float>(levelSize);
        for (uint32_t face = 0; face < 6; ++face) {
            levels[static_cast<size_t>(level) * 6 + face].resize(
                static_cast<size_t>(levelSize) * levelSize);
        }
        parallelRows(levelSize * 6, workers, [&](uint32_t combinedRow) {
            const uint32_t face = combinedRow / levelSize;
            const uint32_t y = combinedRow % levelSize;
            std::vector<glm::vec4> &destination =
                levels[static_cast<size_t>(level) * 6 + face];
            const std::vector<glm::vec4> *previousFaces =
                levels.data() + static_cast<size_t>(level - 1) * 6;
            for (uint32_t x = 0; x < levelSize; ++x) {
                const float sourceX =
                    (static_cast<float>(x) + 0.5f) * scale - 0.5f;
                const float sourceY =
                    (static_cast<float>(y) + 0.5f) * scale - 0.5f;
                destination[static_cast<size_t>(y) * levelSize + x] =
                    sampleCubeLanczos4(
                        previousFaces, previousSize, face, sourceX,
                        sourceY, scale);
            }
        });
        checkCancelled(options);
        previousSize = levelSize;
    }
    for (size_t index = 0; index < levels.size(); ++index)
        image.subresources[index] = packHalf(levels[index], 4);
    return image;
}

BakedImage bakeIrradiance(const HdrImage &source, uint32_t size,
                          uint32_t samples, uint32_t workers,
                          const EnvironmentCacheBuildOptions &options) {
    BakedImage image;
    image.kind = EnvironmentMapKind::Irradiance;
    image.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    image.formatName = "rgba16f";
    image.width = size;
    image.height = size;
    image.faces = 6;
    image.channels = 4;
    image.subresources.resize(6);
    std::vector<std::vector<glm::vec4>> faces(6);
    for (auto &face : faces)
        face.resize(static_cast<size_t>(size) * size);
    parallelRows(size * 6, workers, [&](uint32_t combinedRow) {
        const uint32_t face = combinedRow / size;
        const uint32_t y = combinedRow % size;
        std::vector<glm::vec4> &pixels = faces[face];
        for (uint32_t x = 0; x < size; ++x) {
            const float u =
                (2.0f * (static_cast<float>(x) + 0.5f) / size) - 1.0f;
            const float v =
                (2.0f * (static_cast<float>(y) + 0.5f) / size) - 1.0f;
            const glm::vec3 normal =
                environmentCubeDirection(face, u, v);
            glm::vec3 sum(0.0f);
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const glm::vec3 direction = localToWorld(
                    cosineHemisphere(
                        environmentHammersley(sample, samples)),
                    normal);
                sum += sampleEquirectangular(source, direction);
            }
            // Cosine-weighted Monte Carlo directly estimates
            // (1 / pi) * integral(L * N.L).
            pixels[static_cast<size_t>(y) * size + x] =
                glm::vec4(sum / static_cast<float>(samples), 1.0f);
        }
    });
    checkCancelled(options);
    for (uint32_t face = 0; face < 6; ++face)
        image.subresources[face] = packHalf(faces[face], 4);
    return image;
}

BakedImage bakePrefiltered(const HdrImage &source, uint32_t size,
                           uint32_t samples, uint32_t workers,
                           const EnvironmentCacheBuildOptions &options) {
    BakedImage image;
    image.kind = EnvironmentMapKind::PrefilteredSpecular;
    image.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    image.formatName = "rgba16f";
    image.width = size;
    image.height = size;
    image.mipLevels = fullMipCount(size);
    image.faces = 6;
    image.channels = 4;
    image.subresources.resize(
        static_cast<size_t>(image.mipLevels) * image.faces);
    for (uint32_t level = 0; level < image.mipLevels; ++level) {
        const uint32_t levelSize = std::max(1u, size >> level);
        const float roughness =
            image.mipLevels == 1
                ? 0.0f
                : static_cast<float>(level) /
                      static_cast<float>(image.mipLevels - 1);
        std::vector<std::vector<glm::vec4>> faces(6);
        for (auto &face : faces)
            face.resize(static_cast<size_t>(levelSize) * levelSize);
        parallelRows(levelSize * 6, workers, [&](uint32_t combinedRow) {
            const uint32_t face = combinedRow / levelSize;
            const uint32_t y = combinedRow % levelSize;
            std::vector<glm::vec4> &pixels = faces[face];
            for (uint32_t x = 0; x < levelSize; ++x) {
                const float u =
                    (2.0f * (static_cast<float>(x) + 0.5f) / levelSize) -
                    1.0f;
                const float v =
                    (2.0f * (static_cast<float>(y) + 0.5f) / levelSize) -
                    1.0f;
                const glm::vec3 normal =
                    environmentCubeDirection(face, u, v);
                const glm::vec3 view = normal;
                glm::vec3 sum(0.0f);
                float weight = 0.0f;
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const glm::vec3 halfVector = importanceSampleGgx(
                        environmentHammersley(sample, samples),
                        std::max(roughness, 0.001f), normal);
                    const glm::vec3 light =
                        glm::normalize(2.0f * glm::dot(view, halfVector) *
                                           halfVector -
                                       view);
                    const float nDotL =
                        std::max(glm::dot(normal, light), 0.0f);
                    if (nDotL > 0.0f) {
                        sum += sampleEquirectangular(source, light) * nDotL;
                        weight += nDotL;
                    }
                }
                pixels[static_cast<size_t>(y) * levelSize + x] =
                    glm::vec4(weight > 0.0f ? sum / weight : glm::vec3(0.0f),
                              1.0f);
            }
        });
        checkCancelled(options);
        for (uint32_t face = 0; face < 6; ++face) {
            image.subresources[static_cast<size_t>(level) * 6 + face] =
                packHalf(faces[face], 4);
        }
    }
    return image;
}

BakedImage bakeBrdfLut(uint32_t size, uint32_t samples, uint32_t workers,
                       const EnvironmentCacheBuildOptions &options) {
    BakedImage image;
    image.kind = EnvironmentMapKind::BrdfLut;
    image.vkFormat = VK_FORMAT_R16G16_SFLOAT;
    image.formatName = "rg16f";
    image.width = size;
    image.height = size;
    image.faces = 1;
    image.channels = 2;
    std::vector<glm::vec4> pixels(static_cast<size_t>(size) * size);
    parallelRows(size, workers, [&](uint32_t y) {
        const float roughness =
            (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (uint32_t x = 0; x < size; ++x) {
            const float nDotV =
                (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const glm::vec3 view(
                std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f,
                nDotV);
            float scale = 0.0f;
            float bias = 0.0f;
            const glm::vec3 normal(0.0f, 0.0f, 1.0f);
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const glm::vec3 halfVector = importanceSampleGgx(
                    environmentHammersley(sample, samples), roughness,
                    normal);
                const glm::vec3 light =
                    glm::normalize(2.0f * glm::dot(view, halfVector) *
                                       halfVector -
                                   view);
                const float nDotL = std::max(light.z, 0.0f);
                const float nDotH = std::max(halfVector.z, 0.0f);
                const float vDotH =
                    std::max(glm::dot(view, halfVector), 0.0f);
                if (nDotL > 0.0f) {
                    const float geometry =
                        geometrySmith(nDotV, nDotL, roughness);
                    const float visibility =
                        geometry * vDotH /
                        std::max(nDotH * nDotV, 1e-5f);
                    const float fresnel =
                        std::pow(1.0f - vDotH, 5.0f);
                    scale += (1.0f - fresnel) * visibility;
                    bias += fresnel * visibility;
                }
            }
            pixels[static_cast<size_t>(y) * size + x] =
                glm::vec4(scale / samples, bias / samples, 0.0f, 0.0f);
        }
    });
    checkCancelled(options);
    image.subresources.push_back(packHalf(pixels, 2));
    return image;
}

void checkKtx(KTX_error_code result, const char *operation) {
    if (result != KTX_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 ktxErrorString(result));
    }
}

void writeKtx(const BakedImage &image,
              const std::filesystem::path &path) {
    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = image.vkFormat;
    createInfo.baseWidth = image.width;
    createInfo.baseHeight = image.height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = image.mipLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = image.faces;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;
    ktxTexture2 *texture = nullptr;
    checkKtx(ktxTexture2_Create(&createInfo,
                                KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture),
             "could not create KTX2");
    std::unique_ptr<ktxTexture2, void (*)(ktxTexture2 *)> owner(
        texture, [](ktxTexture2 *value) {
            ktxTexture_Destroy(ktxTexture(value));
        });
    for (uint32_t level = 0; level < image.mipLevels; ++level) {
        for (uint32_t face = 0; face < image.faces; ++face) {
            const std::vector<uint16_t> &bytes =
                image.subresources[static_cast<size_t>(level) * image.faces +
                                   face];
            checkKtx(ktxTexture_SetImageFromMemory(
                         ktxTexture(texture), level, 0, face,
                         reinterpret_cast<const uint8_t *>(bytes.data()),
                         bytes.size() * sizeof(uint16_t)),
                     "could not populate KTX2 image");
        }
    }
    checkKtx(ktxTexture2_DeflateZstd(texture, 9),
             "could not Zstd-compress KTX2");
    checkKtx(ktxTexture2_WriteToNamedFile(texture, path.string().c_str()),
             "could not write KTX2");
}

bool validKtx(const std::filesystem::path &path,
              const BakedImage *expected = nullptr) {
    ktxTexture2 *texture = nullptr;
    const KTX_error_code result = ktxTexture2_CreateFromNamedFile(
        path.string().c_str(), KTX_TEXTURE_CREATE_NO_FLAGS, &texture);
    if (result != KTX_SUCCESS)
        return false;
    std::unique_ptr<ktxTexture2, void (*)(ktxTexture2 *)> owner(
        texture, [](ktxTexture2 *value) {
            ktxTexture_Destroy(ktxTexture(value));
        });
    return !expected ||
           (texture->baseWidth == expected->width &&
            texture->baseHeight == expected->height &&
            texture->numLevels == expected->mipLevels &&
            texture->numFaces == expected->faces &&
            texture->vkFormat == expected->vkFormat);
}

void replaceFile(const std::filesystem::path &temporary,
                 const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            "could not atomically publish environment KTX2 (error " +
            std::to_string(GetLastError()) + ")");
    }
}

std::string cacheKey(const EnvironmentCacheBuildOptions &options,
                     const std::string &sourceSha,
                     EnvironmentMapKind kind) {
    const EnvironmentProfile &profile = options.profile;
    return sha256String(
        sourceSha + "|" + environmentMapKindName(kind) + "|" +
        kAlgorithmVersion + "|" + std::to_string(profile.radianceSize) +
        "|" + std::to_string(profile.irradianceSize) + "|" +
        std::to_string(profile.prefilteredSize) + "|" +
        std::to_string(profile.brdfLutSize) + "|" +
        std::to_string(profile.diffuseSamples) + "|" +
        std::to_string(profile.specularSamples) + "|" +
        std::to_string(profile.brdfSamples) + "|zstd=9|z-up");
}

DerivedEnvironmentImage publishImage(
    const EnvironmentCacheBuildOptions &options,
    const std::string &sourceSha, const BakedImage &image,
    EnvironmentCacheBuildReport &report) {
    const std::string key = cacheKey(options, sourceSha, image.kind);
    const std::string relative = "blobs/" + key + ".ktx2";
    const std::filesystem::path destination = options.cacheRoot / relative;
    std::filesystem::create_directories(destination.parent_path());
    if (!options.force && std::filesystem::is_regular_file(destination) &&
        validKtx(destination, &image)) {
        ++report.reusedBlobs;
    } else {
        const std::filesystem::path temporary =
            destination.string() + ".tmp-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            writeKtx(image, temporary);
            if (!validKtx(temporary, &image))
                throw std::runtime_error(
                    "environment KTX2 validation failed");
            replaceFile(temporary, destination);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        ++report.generatedBlobs;
    }
    const uint64_t bytes = std::filesystem::file_size(destination);
    report.blobBytes += bytes;
    return {image.kind, image.formatName, image.width, image.height,
            image.mipLevels, image.faces, key, relative, bytes};
}

std::optional<DerivedEnvironmentImage> reusePublishedImage(
    const EnvironmentCacheBuildOptions &options,
    const std::string &sourceSha, const BakedImage &expected,
    EnvironmentCacheBuildReport &report) {
    if (options.force)
        return std::nullopt;
    const std::string key =
        cacheKey(options, sourceSha, expected.kind);
    const std::string relative = "blobs/" + key + ".ktx2";
    const std::filesystem::path destination =
        options.cacheRoot / relative;
    if (!std::filesystem::is_regular_file(destination) ||
        !validKtx(destination, &expected)) {
        return std::nullopt;
    }
    const uint64_t bytes = std::filesystem::file_size(destination);
    ++report.reusedBlobs;
    report.blobBytes += bytes;
    return DerivedEnvironmentImage{
        expected.kind,      expected.formatName, expected.width,
        expected.height,    expected.mipLevels,  expected.faces,
        key,                relative,            bytes};
}

} // namespace

glm::vec3 environmentCubeDirection(uint32_t face, float u, float v) {
    glm::vec3 direction;
    switch (face) {
    case 0:
        direction = {1.0f, -v, -u};
        break;
    case 1:
        direction = {-1.0f, -v, u};
        break;
    case 2:
        direction = {u, 1.0f, v};
        break;
    case 3:
        direction = {u, -1.0f, -v};
        break;
    case 4:
        direction = {u, -v, 1.0f};
        break;
    case 5:
        direction = {-u, -v, -1.0f};
        break;
    default:
        throw std::out_of_range("cubemap face index must be in [0, 5]");
    }
    return glm::normalize(direction);
}

glm::vec2 environmentHammersley(uint32_t index, uint32_t count) {
    uint32_t bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) |
           ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) |
           ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) |
           ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) |
           ((bits & 0xFF00FF00u) >> 8u);
    const float radicalInverse =
        static_cast<float>(bits) * 2.3283064365386963e-10f;
    return {static_cast<float>(index) / static_cast<float>(count),
            radicalInverse};
}

EnvironmentCacheBuildReport
buildEnvironmentCache(const EnvironmentCacheBuildOptions &options) {
    if (!std::filesystem::is_regular_file(options.source))
        throw std::invalid_argument("environment source is missing");
    std::string extension = options.source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char value) {
                       return static_cast<char>(std::tolower(
                           static_cast<unsigned char>(value)));
                   });
    if (extension != ".hdr")
        throw std::invalid_argument("environment source must be .hdr");
    if (options.projectId.empty() || options.environmentId.empty() ||
        options.profile.id.empty())
        throw std::invalid_argument(
            "environment cache identity is incomplete");

    EnvironmentCacheBuildReport report;
    report.sourceBytes = std::filesystem::file_size(options.source);
    report.manifestPath = derivedEnvironmentManifestPath(
        options.cacheRoot, options.environmentId, options.profile.id);
    const std::string sourceSha = sha256File(options.source);
    const uint32_t workers = workerCount(options.maxWorkers);
    std::optional<HdrImage> decodedSource;
    const auto source = [&]() -> const HdrImage & {
        if (!decodedSource)
            decodedSource = loadHdr(options.source);
        return *decodedSource;
    };

    DerivedEnvironmentManifest manifest;
    manifest.projectId = options.projectId;
    manifest.environmentId = options.environmentId;
    manifest.profileId = options.profile.id;
    manifest.sourcePath = options.sourceProjectPath.generic_string();
    manifest.sourceSha256 = sourceSha;
    manifest.algorithmVersion = kAlgorithmVersion;
    manifest.diffuseSamples = options.profile.diffuseSamples;
    manifest.specularSamples = options.profile.specularSamples;
    manifest.brdfSamples = options.profile.brdfSamples;
    manifest.source = fileStamp(options.source, sourceSha);
    manifest.source.path = options.sourceProjectPath.generic_string();

    const auto appendImage =
        [&](const BakedImage &expected, auto &&bake) {
            checkCancelled(options);
            if (auto reused = reusePublishedImage(
                    options, sourceSha, expected, report)) {
                manifest.images.push_back(std::move(*reused));
                return;
            }
            BakedImage baked = bake();
            manifest.images.push_back(
                publishImage(options, sourceSha, baked, report));
        };

    appendImage(
        environmentImageDescriptor(EnvironmentMapKind::Radiance,
                                   options.profile.radianceSize),
        [&] {
            return bakeRadiance(
                source(), options.profile.radianceSize, workers, options);
        });
    appendImage(
        environmentImageDescriptor(EnvironmentMapKind::Irradiance,
                                   options.profile.irradianceSize),
        [&] {
            return bakeIrradiance(
                source(), options.profile.irradianceSize,
                options.profile.diffuseSamples, workers, options);
        });
    appendImage(
        environmentImageDescriptor(
            EnvironmentMapKind::PrefilteredSpecular,
            options.profile.prefilteredSize),
        [&] {
            return bakePrefiltered(
                source(), options.profile.prefilteredSize,
                options.profile.specularSamples, workers, options);
        });
    appendImage(
        environmentImageDescriptor(EnvironmentMapKind::BrdfLut,
                                   options.profile.brdfLutSize),
        [&] {
            return bakeBrdfLut(
                options.profile.brdfLutSize,
                options.profile.brdfSamples, workers, options);
        });

    std::string saveError;
    if (!saveDerivedEnvironmentManifest(report.manifestPath, manifest,
                                        saveError)) {
        throw std::runtime_error(
            "could not publish environment manifest: " + saveError);
    }
    return report;
}

} // namespace vkr::assettool
