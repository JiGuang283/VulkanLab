#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

namespace vkr::editor {

struct ReflectionProbeFaceCamera {
    glm::vec3 forward{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
};

const std::array<ReflectionProbeFaceCamera, 6> &
reflectionProbeFaceCameras();

glm::mat4 reflectionProbeFaceView(uint32_t face,
                                  const glm::vec3 &position);

void stitchReflectionProbeFaces(
    const std::array<std::filesystem::path, 6> &faces,
    const std::filesystem::path &output, uint32_t faceSize);

} // namespace vkr::editor
