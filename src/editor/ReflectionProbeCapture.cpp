#include "ReflectionProbeCapture.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vkr::editor {
namespace {

using FloatPixels =
    std::unique_ptr<float, decltype(&stbi_image_free)>;

struct LoadedFace {
    FloatPixels pixels{nullptr, stbi_image_free};
    uint32_t size = 0;
};

LoadedFace loadFace(const std::filesystem::path &path,
                    uint32_t expectedSize) {
    int width = 0;
    int height = 0;
    int channels = 0;
    float *pixels = stbi_loadf(path.string().c_str(), &width, &height,
                               &channels, 3);
    if (!pixels)
        throw std::runtime_error("Could not read probe face '" +
                                 path.string() + "'");
    LoadedFace face{FloatPixels(pixels, stbi_image_free), expectedSize};
    if (width != static_cast<int>(expectedSize) ||
        height != static_cast<int>(expectedSize)) {
        throw std::runtime_error("Probe face has an unexpected extent: " +
                                 path.string());
    }
    return face;
}

glm::vec3 sampleFace(const LoadedFace &face, float u, float v) {
    const float x = std::clamp((u * 0.5f + 0.5f) * face.size - 0.5f,
                               0.0f, float(face.size - 1));
    const float y = std::clamp((0.5f - v * 0.5f) * face.size - 0.5f,
                               0.0f, float(face.size - 1));
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1u, face.size - 1u);
    const uint32_t y1 = std::min(y0 + 1u, face.size - 1u);
    const float tx = x - float(x0);
    const float ty = y - float(y0);
    const auto texel = [&](uint32_t px, uint32_t py) {
        const size_t offset =
            (static_cast<size_t>(py) * face.size + px) * 3u;
        return glm::vec3(face.pixels.get()[offset + 0u],
                         face.pixels.get()[offset + 1u],
                         face.pixels.get()[offset + 2u]);
    };
    return glm::mix(glm::mix(texel(x0, y0), texel(x1, y0), tx),
                    glm::mix(texel(x0, y1), texel(x1, y1), tx), ty);
}

} // namespace

const std::array<ReflectionProbeFaceCamera, 6> &
reflectionProbeFaceCameras() {
    static const std::array<ReflectionProbeFaceCamera, 6> cameras = {{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}},
    }};
    return cameras;
}

glm::mat4 reflectionProbeFaceView(uint32_t face,
                                  const glm::vec3 &position) {
    if (face >= reflectionProbeFaceCameras().size())
        throw std::out_of_range("Reflection probe face index is invalid");
    const ReflectionProbeFaceCamera &camera =
        reflectionProbeFaceCameras()[face];
    return glm::lookAtRH(position, position + camera.forward, camera.up);
}

void stitchReflectionProbeFaces(
    const std::array<std::filesystem::path, 6> &facePaths,
    const std::filesystem::path &output, uint32_t faceSize) {
    if (faceSize == 0)
        throw std::invalid_argument("Reflection probe face size is zero");
    std::array<LoadedFace, 6> faces;
    for (uint32_t face = 0; face < faces.size(); ++face)
        faces[face] = loadFace(facePaths[face], faceSize);

    const uint32_t width = faceSize * 4u;
    const uint32_t height = faceSize * 2u;
    std::vector<float> pixels(static_cast<size_t>(width) * height * 3u);
    const auto &cameras = reflectionProbeFaceCameras();
    for (uint32_t y = 0; y < height; ++y) {
        const float theta =
            (float(y) + 0.5f) / float(height) * glm::pi<float>();
        const float sinTheta = std::sin(theta);
        for (uint32_t x = 0; x < width; ++x) {
            const float phi =
                ((float(x) + 0.5f) / float(width) - 0.5f) *
                glm::two_pi<float>();
            const glm::vec3 sourceDirection(
                sinTheta * std::cos(phi), std::cos(theta),
                sinTheta * std::sin(phi));
            const glm::vec3 direction(sourceDirection.x,
                                      -sourceDirection.z,
                                      sourceDirection.y);

            uint32_t bestFace = 0;
            float bestForward = -1.0f;
            for (uint32_t face = 0; face < cameras.size(); ++face) {
                const float forward =
                    glm::dot(direction, cameras[face].forward);
                if (forward > bestForward) {
                    bestForward = forward;
                    bestFace = face;
                }
            }
            const ReflectionProbeFaceCamera &camera = cameras[bestFace];
            const glm::vec3 right =
                glm::normalize(glm::cross(camera.forward, camera.up));
            const float u = glm::dot(direction, right) / bestForward;
            const float v = glm::dot(direction, camera.up) / bestForward;
            const glm::vec3 color =
                glm::max(sampleFace(faces[bestFace], u, v),
                         glm::vec3(0.0f));
            const size_t offset =
                (static_cast<size_t>(y) * width + x) * 3u;
            pixels[offset + 0u] = std::isfinite(color.r) ? color.r : 0.0f;
            pixels[offset + 1u] = std::isfinite(color.g) ? color.g : 0.0f;
            pixels[offset + 2u] = std::isfinite(color.b) ? color.b : 0.0f;
        }
    }

    std::filesystem::create_directories(output.parent_path());
    if (!stbi_write_hdr(output.string().c_str(), static_cast<int>(width),
                        static_cast<int>(height), 3, pixels.data())) {
        throw std::runtime_error("Could not write reflection probe HDR: " +
                                 output.string());
    }
}

} // namespace vkr::editor
