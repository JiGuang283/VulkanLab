#include "render/Visibility.h"

#include "diagnostics/Profiling.h"
#include "render/RenderView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vkr {

namespace {

glm::vec4 matrixRow(const glm::mat4 &matrix, uint32_t row) {
    return {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
}

bool finiteVec3(const glm::vec3 &value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

std::array<glm::vec3, 8> boundsCorners(const Bounds &bounds) {
    std::array<glm::vec3, 8> corners{};
    uint32_t index = 0;
    for (uint32_t z = 0; z < 2; ++z) {
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                corners[index++] = {
                    x ? bounds.max.x : bounds.min.x,
                    y ? bounds.max.y : bounds.min.y,
                    z ? bounds.max.z : bounds.min.z};
            }
        }
    }
    return corners;
}

float distanceSquaredToBounds(const glm::vec3 &point, const Bounds &bounds) {
    const glm::vec3 closest = glm::clamp(point, bounds.min, bounds.max);
    const glm::vec3 delta = point - closest;
    return glm::dot(delta, delta);
}

bool intersectsDirectionalShadowVolume(
    const Bounds &worldBounds,
    const DirectionalShadowFrameData &shadow) {
    if (!worldBounds.valid || !shadow.enabled)
        return true;
    const Bounds lightBounds = transformBounds(worldBounds, shadow.lightView);
    if (!lightBounds.valid)
        return true;
    return lightBounds.max.x >= shadow.lightSpaceMin.x &&
           lightBounds.min.x <= shadow.lightSpaceMax.x &&
           lightBounds.max.y >= shadow.lightSpaceMin.y &&
           lightBounds.min.y <= shadow.lightSpaceMax.y &&
           lightBounds.max.z >= shadow.lightSpaceMin.z &&
           lightBounds.min.z <= shadow.lightSpaceMax.z;
}

bool projectedObjectIsSmall(const Bounds &bounds, const glm::mat4 &viewProj,
                            VkExtent2D viewportExtent, float thresholdPixels) {
    if (!bounds.valid || viewportExtent.width == 0 ||
        viewportExtent.height == 0 || thresholdPixels <= 0.0f) {
        return false;
    }

    glm::vec2 minPixel(std::numeric_limits<float>::max());
    glm::vec2 maxPixel(std::numeric_limits<float>::lowest());
    for (const glm::vec3 &corner : boundsCorners(bounds)) {
        const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
            !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
            clip.w <= 1.0e-5f || clip.z < 0.0f) {
            return false;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 pixel =
            (ndc * 0.5f + 0.5f) *
            glm::vec2(static_cast<float>(viewportExtent.width),
                      static_cast<float>(viewportExtent.height));
        minPixel = glm::min(minPixel, pixel);
        maxPixel = glm::max(maxPixel, pixel);
    }

    const glm::vec2 viewportSize(viewportExtent.width, viewportExtent.height);
    minPixel = glm::clamp(minPixel, glm::vec2(0.0f), viewportSize);
    maxPixel = glm::clamp(maxPixel, glm::vec2(0.0f), viewportSize);
    const glm::vec2 extent = glm::max(maxPixel - minPixel, glm::vec2(0.0f));
    return extent.x < thresholdPixels && extent.y < thresholdPixels;
}

RenderItemId fallbackRenderItemId(const RenderCommand &command) {
    uint64_t value = static_cast<uint64_t>(command.sourceOrder) + 1u;
    value ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(command.mesh));
    value *= 1099511628211ull;
    return value;
}

} // namespace

Frustum Frustum::fromVulkanClipMatrix(const glm::mat4 &matrix) {
    Frustum result{};
    const glm::vec4 row0 = matrixRow(matrix, 0);
    const glm::vec4 row1 = matrixRow(matrix, 1);
    const glm::vec4 row2 = matrixRow(matrix, 2);
    const glm::vec4 row3 = matrixRow(matrix, 3);
    result.planes = {row3 + row0, row3 - row0, row3 + row1,
                     row3 - row1, row2, row3 - row2};
    for (glm::vec4 &plane : result.planes) {
        const float length = glm::length(glm::vec3(plane));
        if (!std::isfinite(length) || length <= 1.0e-7f)
            return {};
        plane /= length;
    }
    result.valid = true;
    return result;
}

bool Frustum::intersects(const Bounds &bounds) const {
    if (!valid || !bounds.valid)
        return true;
    const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
    const glm::vec3 extent = glm::max((bounds.max - bounds.min) * 0.5f,
                                      glm::vec3(0.0f));
    for (const glm::vec4 &plane : planes) {
        const glm::vec3 normal(plane);
        const float radius = glm::dot(glm::abs(normal), extent);
        if (glm::dot(normal, center) + plane.w + radius < 0.0f)
            return false;
    }
    return true;
}

Bounds transformBounds(const Bounds &localBounds, const glm::mat4 &world) {
    Bounds result{};
    if (!localBounds.valid)
        return result;

    const glm::vec3 localCenter =
        (localBounds.min + localBounds.max) * 0.5f;
    const glm::vec3 localExtent =
        glm::max((localBounds.max - localBounds.min) * 0.5f,
                 glm::vec3(0.0f));
    const glm::vec4 transformedCenter = world * glm::vec4(localCenter, 1.0f);
    glm::mat3 absoluteLinear(world);
    for (uint32_t column = 0; column < 3; ++column) {
        for (uint32_t row = 0; row < 3; ++row)
            absoluteLinear[column][row] =
                std::abs(absoluteLinear[column][row]);
    }
    const glm::vec3 worldExtent = absoluteLinear * localExtent;
    const glm::vec3 center(transformedCenter);
    if (!finiteVec3(center) || !finiteVec3(worldExtent))
        return result;

    result.min = center - worldExtent;
    result.max = center + worldExtent;
    result.center = center;
    result.radius = glm::length(worldExtent);
    result.valid = finiteVec3(result.min) && finiteVec3(result.max) &&
                   std::isfinite(result.radius);
    return result;
}

VisibilityFrame VisibilitySystem::build(const RenderQueue &source,
                                        const RenderView &view,
                                        VkExtent2D viewportExtent) const {
    VKL_PROFILE_ZONE("Build Visibility");
    VisibilityFrame result{};
    const CullingSettings &settings = view.settings.culling;
    const glm::mat4 cameraViewProj = view.globalUbo.proj * view.globalUbo.view;
    const Frustum cameraFrustum =
        Frustum::fromVulkanClipMatrix(cameraViewProj);
    const glm::vec3 cameraPosition(view.globalUbo.cameraPosWS);

    const auto processCamera = [&](const RenderCommand &sourceCommand) {
        ++result.stats.sourceDraws;
        RenderCommand command = sourceCommand;
        command.worldBounds = transformBounds(command.localBounds,
                                              command.world);
        if (command.renderItemId == 0)
            command.renderItemId = fallbackRenderItemId(command);
        if (!command.worldBounds.valid) {
            ++result.stats.invalidBounds;
        } else {
            if (settings.frustumEnabled && cameraFrustum.valid &&
                !cameraFrustum.intersects(command.worldBounds)) {
                ++result.stats.frustumCulled;
                return;
            }
            if (settings.distanceEnabled && settings.maxDrawDistance > 0.0f &&
                distanceSquaredToBounds(cameraPosition, command.worldBounds) >
                    settings.maxDrawDistance * settings.maxDrawDistance) {
                ++result.stats.distanceCulled;
                return;
            }
            if (settings.smallObjectEnabled &&
                projectedObjectIsSmall(command.worldBounds, cameraViewProj,
                                       viewportExtent,
                                       settings.minProjectedSizePixels)) {
                ++result.stats.smallObjectCulled;
                return;
            }
        }

        result.camera.add(command);
        ++result.stats.cameraVisible;
        if (command.queue == RenderQueueType::Opaque) {
            ++result.stats.cameraOpaque;
            result.depthPrepass.add(command);
        } else {
            ++result.stats.cameraTransparent;
        }
    };

    for (const RenderCommand &command : source.opaque())
        processCamera(command);
    for (const RenderCommand &command : source.transparent())
        processCamera(command);

    for (const RenderCommand &sourceCommand : source.opaque()) {
        ++result.stats.shadowCandidates;
        RenderCommand command = sourceCommand;
        command.worldBounds = transformBounds(command.localBounds,
                                              command.world);
        if (settings.shadowCullingEnabled &&
            view.directionalShadow.enabled && command.worldBounds.valid &&
            !intersectsDirectionalShadowVolume(
                command.worldBounds, view.directionalShadow)) {
            ++result.stats.shadowCulled;
            continue;
        }
        result.shadowCasters.add(command);
        ++result.stats.shadowVisible;
    }

    result.camera.sortOpaque();
    result.camera.sortTransparent(cameraPosition);
    result.shadowCasters.sortOpaque();
    result.depthPrepass.sortOpaque();
    uint32_t slot = 0;
    for (RenderCommand &command : result.camera.mutableOpaque())
        command.occlusionSlot = slot++;
    result.stats.depthPrepassDraws =
        static_cast<uint32_t>(result.depthPrepass.opaque().size());
    result.stats.occlusionCandidates = slot;
    return result;
}

} // namespace vkr
