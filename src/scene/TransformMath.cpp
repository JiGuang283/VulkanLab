#include "TransformMath.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace vkr {
namespace {

bool finiteMatrix(const glm::mat4 &matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row]))
                return false;
        }
    }
    return true;
}

float maximumMagnitude(const glm::mat4 &matrix) {
    float result = 0.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row)
            result = std::max(result, std::abs(matrix[column][row]));
    }
    return result;
}

float maximumDifference(const glm::mat4 &left, const glm::mat4 &right) {
    float result = 0.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result = std::max(
                result, std::abs(left[column][row] - right[column][row]));
        }
    }
    return result;
}

} // namespace

glm::mat4 composeSceneTransform(const SceneTransformDocument &transform) {
    return glm::translate(glm::mat4(1.0f), transform.translation) *
           glm::mat4_cast(glm::normalize(transform.rotation)) *
           glm::scale(glm::mat4(1.0f), transform.scale);
}

bool decomposeSceneTransform(const glm::mat4 &matrix,
                             SceneTransformDocument &transform,
                             float tolerance) {
    constexpr float kMinimumScale = 1.0e-6f;
    if (!finiteMatrix(matrix) ||
        std::abs(matrix[0][3]) > tolerance ||
        std::abs(matrix[1][3]) > tolerance ||
        std::abs(matrix[2][3]) > tolerance ||
        std::abs(matrix[3][3] - 1.0f) > tolerance) {
        return false;
    }

    glm::vec3 axes[] = {glm::vec3(matrix[0]), glm::vec3(matrix[1]),
                        glm::vec3(matrix[2])};
    glm::vec3 scale{glm::length(axes[0]), glm::length(axes[1]),
                    glm::length(axes[2])};
    if (scale.x < kMinimumScale || scale.y < kMinimumScale ||
        scale.z < kMinimumScale) {
        return false;
    }

    axes[0] /= scale.x;
    axes[1] /= scale.y;
    axes[2] /= scale.z;
    if (std::abs(glm::dot(axes[0], axes[1])) > tolerance ||
        std::abs(glm::dot(axes[0], axes[2])) > tolerance ||
        std::abs(glm::dot(axes[1], axes[2])) > tolerance) {
        return false;
    }

    const float determinant = glm::dot(glm::cross(axes[0], axes[1]), axes[2]);
    if (std::abs(std::abs(determinant) - 1.0f) > tolerance * 4.0f)
        return false;
    if (determinant < 0.0f) {
        int negativeAxis = 0;
        if (scale.y > scale.x)
            negativeAxis = 1;
        if (scale.z > scale[negativeAxis])
            negativeAxis = 2;
        scale[negativeAxis] = -scale[negativeAxis];
        axes[negativeAxis] = -axes[negativeAxis];
    }

    SceneTransformDocument candidate;
    candidate.translation = glm::vec3(matrix[3]);
    candidate.rotation = glm::normalize(
        glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
    candidate.scale = scale;
    if (!std::isfinite(candidate.rotation.w) ||
        !std::isfinite(candidate.rotation.x) ||
        !std::isfinite(candidate.rotation.y) ||
        !std::isfinite(candidate.rotation.z)) {
        return false;
    }

    const glm::mat4 reconstructed = composeSceneTransform(candidate);
    const float allowedError =
        tolerance * std::max(1.0f, maximumMagnitude(matrix));
    if (maximumDifference(matrix, reconstructed) > allowedError)
        return false;

    transform = candidate;
    return true;
}

} // namespace vkr
