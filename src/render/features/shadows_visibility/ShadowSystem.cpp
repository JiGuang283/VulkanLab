#include "render/features/shadows_visibility/ShadowSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <unordered_map>

namespace vkr {
namespace {

constexpr float kReplacementThreshold = 1.25f;

glm::vec3 normalizeOrFallback(const glm::vec3 &value,
                              const glm::vec3 &fallback) {
    const float lengthSquared = glm::dot(value, value);
    return std::isfinite(lengthSquared) && lengthSquared > 1.0e-8f
               ? value / std::sqrt(lengthSquared)
               : glm::normalize(fallback);
}

SceneLight makeFallbackSun(const ShadowBuildInput &input) {
    SceneLight light{};
    light.debugName = "Fallback Sun";
    light.stableKey = "fallback/sun";
    light.source = SceneLightSource::Fallback;
    light.shadowPolicy = ShadowCastingPolicy::Forced;
    light.type = LightType::Directional;
    light.directionWS = normalizeOrFallback(
        input.fallbackSunDirection, glm::vec3(0.3f, 0.8f, 0.5f));
    light.color = glm::max(input.fallbackSunColor, glm::vec3(0.0f));
    light.intensity = std::max(input.fallbackSunIntensity, 0.0f);
    return light;
}

uint32_t sourcePriority(SceneLightSource source) {
    switch (source) {
    case SceneLightSource::ExplicitEntity:
        return 0;
    case SceneLightSource::ImportedModel:
        return 1;
    case SceneLightSource::Legacy:
        return 2;
    case SceneLightSource::Fallback:
        return 3;
    }
    return std::numeric_limits<uint32_t>::max();
}

bool validShadowCandidate(const SceneLight &light) {
    if (light.shadowPolicy == ShadowCastingPolicy::Disabled)
        return false;
    if (!std::isfinite(light.positionWS.x) ||
        !std::isfinite(light.positionWS.y) ||
        !std::isfinite(light.positionWS.z)) {
        return light.type == LightType::Directional;
    }
    if (light.type == LightType::Directional ||
        light.type == LightType::Spot) {
        const float lengthSquared =
            glm::dot(light.directionWS, light.directionWS);
        return std::isfinite(lengthSquared) && lengthSquared > 1.0e-8f;
    }
    return true;
}

float luminance(const glm::vec3 &color) {
    return glm::dot(glm::max(color, glm::vec3(0.0f)),
                    glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

float candidateScore(const SceneLight &light,
                     const ShadowBuildInput &input,
                     float maximumDistance) {
    const float distance =
        std::max(glm::length(light.positionWS - input.cameraPosition),
                 0.1f);
    const float range = light.range > 0.01f
                            ? std::min(light.range, maximumDistance)
                            : maximumDistance;
    float coverage = glm::clamp((range * range) / (distance * distance),
                                0.0f, 1.0f);
    if (light.type == LightType::Spot) {
        const glm::vec3 toCamera =
            normalizeOrFallback(input.cameraPosition - light.positionWS,
                                -light.directionWS);
        const glm::vec3 direction = normalizeOrFallback(
            light.directionWS, glm::vec3(0.0f, -1.0f, 0.0f));
        const float cone = glm::smoothstep(
            light.outerConeCos, std::max(light.innerConeCos,
                                         light.outerConeCos + 0.001f),
            glm::dot(direction, toCamera));
        coverage *= glm::mix(0.1f, 1.0f, cone);
    }
    return std::max(luminance(light.color) *
                        std::max(light.intensity, 0.0f) * coverage,
                    0.0f);
}

uint32_t allocationPriority(const SceneLight &light,
                            const ShadowBuildInput &input) {
    if (input.focusedLightEntity && light.ownerEntity &&
        light.source == SceneLightSource::ExplicitEntity &&
        *input.focusedLightEntity == *light.ownerEntity) {
        return 3;
    }
    if (light.shadowPolicy == ShadowCastingPolicy::Forced)
        return 2;
    return 1;
}

struct Candidate {
    const SceneLight *light = nullptr;
    std::string key;
    float score = 0.0f;
    float farPlane = 0.0f;
    uint32_t priority = 0;
    bool focused = false;
};

template <size_t N>
std::vector<ShadowAllocation> allocateSlots(
    LightType type, std::vector<Candidate> candidates, uint32_t budget,
    std::array<ShadowSlotState, N> &states,
    std::vector<ShadowEviction> &evictions) {
    budget = std::min<uint32_t>(budget, static_cast<uint32_t>(N));
    const auto stronger = [](const Candidate &left,
                             const Candidate &right) {
        if (left.priority != right.priority)
            return left.priority > right.priority;
        if (left.score != right.score)
            return left.score > right.score;
        return left.key < right.key;
    };
    std::sort(candidates.begin(), candidates.end(), stronger);

    std::unordered_map<std::string, const Candidate *> byKey;
    for (const Candidate &candidate : candidates)
        byKey.emplace(candidate.key, &candidate);

    std::array<const Candidate *, N> assigned{};
    std::array<bool, N> retained{};
    std::unordered_map<std::string, uint32_t> assignedKeys;
    for (uint32_t slot = 0; slot < budget; ++slot) {
        const auto found = byKey.find(states[slot].stableKey);
        if (found == byKey.end()) {
            if (!states[slot].stableKey.empty()) {
                evictions.push_back(
                    {type, states[slot].stableKey,
                     "ineligible_or_removed"});
            }
            continue;
        }
        assigned[slot] = found->second;
        retained[slot] = true;
        assignedKeys.emplace(found->first, slot);
    }

    const auto weakestSlot = [&]() -> uint32_t {
        uint32_t result = 0;
        for (uint32_t slot = 1; slot < budget; ++slot) {
            if (!assigned[slot])
                return slot;
            if (!assigned[result])
                continue;
            const Candidate &candidate = *assigned[slot];
            const Candidate &weakest = *assigned[result];
            if (candidate.priority < weakest.priority ||
                (candidate.priority == weakest.priority &&
                 (candidate.score < weakest.score ||
                  (candidate.score == weakest.score &&
                   candidate.key > weakest.key)))) {
                result = slot;
            }
        }
        return result;
    };

    for (const Candidate &candidate : candidates) {
        if (assignedKeys.find(candidate.key) != assignedKeys.end() ||
            budget == 0)
            continue;
        uint32_t empty = budget;
        for (uint32_t slot = 0; slot < budget; ++slot) {
            if (!assigned[slot]) {
                empty = slot;
                break;
            }
        }
        if (empty < budget) {
            assigned[empty] = &candidate;
            retained[empty] = false;
            assignedKeys.emplace(candidate.key, empty);
            continue;
        }

        const uint32_t slot = weakestSlot();
        const Candidate &incumbent = *assigned[slot];
        const bool replace =
            candidate.priority > incumbent.priority ||
            (candidate.priority == incumbent.priority &&
             candidate.score > incumbent.score * kReplacementThreshold);
        if (!replace)
            continue;
        evictions.push_back(
            {type, incumbent.key,
             candidate.priority > incumbent.priority
                 ? "higher_priority"
                 : "contribution_hysteresis"});
        assignedKeys.erase(incumbent.key);
        assigned[slot] = &candidate;
        retained[slot] = false;
        assignedKeys.emplace(candidate.key, slot);
    }

    for (uint32_t slot = budget; slot < N; ++slot) {
        if (!states[slot].stableKey.empty())
            evictions.push_back({type, states[slot].stableKey,
                                 "budget_reduced"});
        states[slot] = {};
    }

    std::vector<ShadowAllocation> result;
    result.reserve(budget);
    for (uint32_t slot = 0; slot < budget; ++slot) {
        const Candidate *candidate = assigned[slot];
        if (!candidate) {
            states[slot] = {};
            continue;
        }
        const bool same = states[slot].stableKey == candidate->key;
        states[slot] = {candidate->key, candidate->score,
                        candidate->priority,
                        same ? states[slot].age + 1u : 1u};
        result.push_back(
            {type,
             slot,
             candidate->light->ownerEntity,
             candidate->key,
             candidate->light->debugName,
             candidate->light->shadowPolicy,
             candidate->score,
             candidate->farPlane,
             states[slot].age,
             same && retained[slot],
             candidate->focused});
    }
    return result;
}

void hashBytes(uint64_t &hash, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void hashValue(uint64_t &hash, const T &value) {
    hashBytes(hash, &value, sizeof(value));
}

void hashString(uint64_t &hash, const std::string &value) {
    hashBytes(hash, value.data(), value.size());
}

uint64_t contentHash(const ShadowFramePlan &plan) {
    uint64_t hash = 1469598103934665603ull;
    hashValue(hash, plan.csm.enabled);
    for (const CsmCascadeData &cascade : plan.csm.cascades) {
        hashValue(hash, cascade.lightViewProjection);
        hashValue(hash, cascade.nearDistance);
        hashValue(hash, cascade.splitDistance);
        hashValue(hash, cascade.blendStartDistance);
        hashValue(hash, cascade.stableRadius);
        hashValue(hash, cascade.worldUnitsPerTexel);
        hashValue(hash, cascade.valid);
    }
    for (size_t index = 0; index < plan.effectiveLights.size(); ++index) {
        const SceneLight &light = plan.effectiveLights[index];
        hashString(hash, sceneLightStableKey(light, index));
        hashValue(hash, light.type);
        hashValue(hash, light.shadowPolicy);
        hashValue(hash, light.positionWS);
        hashValue(hash, light.directionWS);
        hashValue(hash, light.color);
        hashValue(hash, light.intensity);
        hashValue(hash, light.range);
        hashValue(hash, light.innerConeCos);
        hashValue(hash, light.outerConeCos);
    }
    for (const ShadowAllocation &allocation : plan.allocations) {
        hashString(hash, allocation.stableKey);
        hashValue(hash, allocation.type);
        hashValue(hash, allocation.slot);
        hashValue(hash, allocation.farPlane);
    }
    return hash;
}

} // namespace

bool isEffectiveSceneLight(const SceneLight &light) {
    constexpr float kMinimumContribution = 1.0e-5f;
    if (!std::isfinite(light.intensity) ||
        light.intensity <= kMinimumContribution) {
        return false;
    }
    return std::isfinite(light.color.r) && std::isfinite(light.color.g) &&
           std::isfinite(light.color.b) &&
           (light.color.r > kMinimumContribution ||
            light.color.g > kMinimumContribution ||
            light.color.b > kMinimumContribution);
}

std::string sceneLightStableKey(const SceneLight &light,
                                size_t fallbackIndex) {
    if (!light.stableKey.empty())
        return light.stableKey;
    if (light.ownerEntity)
        return "entity/" + light.ownerEntity->toString();
    if (!light.debugName.empty())
        return "legacy/" + light.debugName;
    return "legacy/" + std::to_string(fallbackIndex);
}

ShadowFramePlan ShadowSystem::build(const ShadowBuildInput &input) {
    ShadowFramePlan result{};
    if (input.sceneLights) {
        for (const SceneLight &light : *input.sceneLights) {
            if (isEffectiveSceneLight(light))
                result.effectiveLights.push_back(light);
        }
    }
    if (result.effectiveLights.empty() && input.fallbackSunEnabled)
        result.effectiveLights.push_back(makeFallbackSun(input));

    const SceneLight *directional = nullptr;
    size_t directionalIndex = 0;
    std::vector<Candidate> pointCandidates;
    std::vector<Candidate> spotCandidates;
    for (size_t index = 0; index < result.effectiveLights.size(); ++index) {
        const SceneLight &light = result.effectiveLights[index];
        if (!validShadowCandidate(light))
            continue;
        const std::string key = sceneLightStableKey(light, index);
        if (light.type == LightType::Directional) {
            if (!directional ||
                std::tuple{sourcePriority(light.source), key} <
                    std::tuple{sourcePriority(directional->source),
                               sceneLightStableKey(*directional,
                                                   directionalIndex)}) {
                directional = &light;
                directionalIndex = index;
            }
            continue;
        }

        const float maximumDistance =
            light.type == LightType::Point
                ? input.settings.pointShadowDistance
                : input.settings.spotShadowDistance;
        Candidate candidate{};
        candidate.light = &light;
        candidate.key = key;
        candidate.score = candidateScore(light, input, maximumDistance);
        candidate.farPlane = light.range > 0.01f
                                 ? std::min(light.range, maximumDistance)
                                 : maximumDistance;
        candidate.priority = allocationPriority(light, input);
        candidate.focused = candidate.priority == 3;
        if (light.type == LightType::Point)
            pointCandidates.push_back(std::move(candidate));
        else
            spotCandidates.push_back(std::move(candidate));
    }
    result.statistics.pointCandidates =
        static_cast<uint32_t>(pointCandidates.size());
    result.statistics.spotCandidates =
        static_cast<uint32_t>(spotCandidates.size());

    const uint32_t pointBudget = input.settings.shadowsEnabled
                                     ? std::min(input.settings.maxPointShadowLights,
                                                kMaxPointShadowLights)
                                     : 0u;
    const uint32_t spotBudget = input.settings.shadowsEnabled
                                    ? std::min(input.settings.maxSpotShadowLights,
                                               kMaxSpotShadowLights)
                                    : 0u;
    auto pointAllocations = allocateSlots(
        LightType::Point, std::move(pointCandidates), pointBudget,
        pointSlots_, result.statistics.evictions);
    auto spotAllocations = allocateSlots(
        LightType::Spot, std::move(spotCandidates), spotBudget,
        spotSlots_, result.statistics.evictions);

    if (directional) {
        result.directionalEntity = directional->ownerEntity;
        result.directionalStableKey =
            sceneLightStableKey(*directional, directionalIndex);
        result.directionalName = directional->debugName;
        const DirectionalShadowCameraData camera{
            input.cameraView, input.cameraProjection,
            input.cameraNearPlane, input.cameraFarPlane,
            input.settings.culling.shadowDistance};
        result.csm = buildCsmFrameData(
            input.sceneBounds, directional,
            input.settings.shadowsEnabled, camera);
        if (result.csm.enabled) {
            result.allocations.push_back(
                {LightType::Directional, 0, directional->ownerEntity,
                 result.directionalStableKey, directional->debugName,
                 directional->shadowPolicy, 0.0f, 0.0f, 1, true,
                 input.focusedLightEntity && directional->ownerEntity &&
                     *input.focusedLightEntity == *directional->ownerEntity});
            result.lightBindings.emplace(
                result.directionalStableKey,
                ShadowLightBinding{LightType::Directional, 0, 0.0f});
        }
    }

    const auto findLight = [&result](const std::string &key)
        -> const SceneLight * {
        for (size_t index = 0; index < result.effectiveLights.size(); ++index) {
            if (sceneLightStableKey(result.effectiveLights[index], index) == key)
                return &result.effectiveLights[index];
        }
        return nullptr;
    };
    for (const ShadowAllocation &allocation : pointAllocations) {
        const SceneLight *light = findLight(allocation.stableKey);
        if (!light)
            continue;
        PointShadowLightInput shadowInput{};
        shadowInput.position = light->positionWS;
        shadowInput.farPlane = allocation.farPlane;
        result.punctual.points[allocation.slot] = buildPointShadowData(
            shadowInput, allocation.slot * kPointShadowFaceCount,
            kPointShadowMapSize);
        result.lightBindings.emplace(
            allocation.stableKey,
            ShadowLightBinding{LightType::Point,
                               static_cast<int32_t>(allocation.slot),
                               allocation.farPlane});
        result.allocations.push_back(allocation);
    }
    for (const ShadowAllocation &allocation : spotAllocations) {
        const SceneLight *light = findLight(allocation.stableKey);
        if (!light)
            continue;
        SpotShadowLightInput shadowInput{};
        shadowInput.position = light->positionWS;
        shadowInput.direction = light->directionWS;
        shadowInput.fovY =
            std::acos(glm::clamp(light->outerConeCos, -1.0f, 1.0f)) *
            2.0f;
        shadowInput.nearPlane = 0.1f;
        shadowInput.farPlane = allocation.farPlane;
        result.punctual.spots[allocation.slot] = buildSpotShadowData(
            shadowInput, allocation.slot, kSpotShadowMapSize);
        result.lightBindings.emplace(
            allocation.stableKey,
            ShadowLightBinding{LightType::Spot,
                               static_cast<int32_t>(allocation.slot),
                               allocation.farPlane});
        result.allocations.push_back(allocation);
    }
    for (const ShadowAllocation &allocation : pointAllocations) {
        result.punctual.activePointCount =
            std::max(result.punctual.activePointCount,
                     allocation.slot + 1u);
    }
    for (const ShadowAllocation &allocation : spotAllocations) {
        result.punctual.activeSpotCount =
            std::max(result.punctual.activeSpotCount,
                     allocation.slot + 1u);
    }

    uint64_t hash = contentHash(result);
    hashValue(hash, input.settings.shadowsEnabled);
    hashValue(hash, input.settings.shadowReceiverBias);
    hashValue(hash, input.settings.pointShadowReceiverBiasWorld);
    hashValue(hash, input.settings.shadowConstantBias);
    hashValue(hash, input.settings.shadowSlopeBias);
    hashValue(hash, input.settings.maxPointShadowLights);
    hashValue(hash, input.settings.maxSpotShadowLights);
    hashValue(hash, input.settings.pointShadowDistance);
    hashValue(hash, input.settings.spotShadowDistance);
    hashValue(hash, input.settings.culling.shadowDistance);
    const bool changed = previousContentHash_ != 0 &&
                         previousContentHash_ != hash;
    if (previousContentHash_ == 0 || changed)
        ++contentRevision_;
    if (changed)
        reactiveFramesRemaining_ = 2;
    result.temporalReactive = changed || reactiveFramesRemaining_ > 0;
    if (!changed && reactiveFramesRemaining_ > 0)
        --reactiveFramesRemaining_;
    result.contentRevision = contentRevision_;
    result.statistics.reactiveFramesRemaining =
        reactiveFramesRemaining_;
    previousContentHash_ = hash;
    return result;
}

void ShadowSystem::reset() {
    pointSlots_ = {};
    spotSlots_ = {};
    previousContentHash_ = 0;
    contentRevision_ = 0;
    reactiveFramesRemaining_ = 0;
}

} // namespace vkr
