#ifndef VKL_DDGI_SAMPLING_GLSL
#define VKL_DDGI_SAMPLING_GLSL

struct DdgiProbeStateSample {
    vec4 offsetClassification;
    vec4 statistics;
};
layout(std140, set = 5, binding = 0) uniform DdgiSamplingParameters {
    mat4 localToWorld;
    mat4 worldToLocal;
    uvec4 probeCounts;
    vec4 probeSpacingMaxDistance;
    vec4 updateParameters;
    uvec4 runtimeParameters;
    uvec4 updateWindow;
    vec4 traceParameters;
} ddgiSampling;
layout(set = 5, binding = 1) uniform sampler2DArray ddgiIrradiance;
layout(set = 5, binding = 2) uniform sampler2DArray ddgiDistanceMoments;
layout(std430, set = 5, binding = 3) readonly buffer DdgiProbeStates {
    DdgiProbeStateSample states[];
} ddgiProbeStates;

vec2 ddgiSampleOctEncode(vec3 direction)
{
    direction /= max(abs(direction.x) + abs(direction.y) +
                     abs(direction.z), 1.0e-6);
    vec2 encoded = direction.xy;
    if (direction.z < 0.0)
        encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return encoded * 0.5 + 0.5;
}

uint ddgiSampleProbeIndex(ivec3 coord)
{
    return uint(coord.x) + ddgiSampling.probeCounts.x *
           (uint(coord.y) + ddgiSampling.probeCounts.y * uint(coord.z));
}

vec3 ddgiSampleProbeLocalPosition(ivec3 coord, vec3 offset)
{
    vec3 center = (vec3(ddgiSampling.probeCounts.xyz) - 1.0) * 0.5;
    return (vec3(coord) - center) *
           ddgiSampling.probeSpacingMaxDistance.xyz + offset;
}

bool ddgiIsActive()
{
    return ddgiSampling.probeCounts.w > 0u &&
           ddgiSampling.updateParameters.w > 0.0;
}

vec3 evaluateDdgiDebug(vec3 positionWS, vec3 normalWS)
{
    if (!ddgiIsActive() || ddgiSampling.updateWindow.w == 0u)
        return vec3(0.0);
    vec3 localPosition = (ddgiSampling.worldToLocal *
                          vec4(positionWS, 1.0)).xyz;
    vec3 center = (vec3(ddgiSampling.probeCounts.xyz) - 1.0) * 0.5;
    ivec3 coord = ivec3(round(localPosition /
                              ddgiSampling.probeSpacingMaxDistance.xyz +
                              center));
    coord = clamp(coord, ivec3(0),
                  ivec3(ddgiSampling.probeCounts.xyz) - ivec3(1));
    uint index = ddgiSampleProbeIndex(coord);
    DdgiProbeStateSample state = ddgiProbeStates.states[index];
    vec3 localNormal = normalize(mat3(ddgiSampling.worldToLocal) * normalWS);
    if (ddgiSampling.updateWindow.w == 1u) {
        return texture(ddgiIrradiance,
                       vec3(ddgiSampleOctEncode(localNormal),
                            float(index))).rgb;
    }
    if (ddgiSampling.updateWindow.w == 2u) {
        vec3 probeLocal = ddgiSampleProbeLocalPosition(
            coord, state.offsetClassification.xyz);
        vec3 direction = localPosition - probeLocal;
        direction = length(direction) > 1.0e-5
                        ? normalize(direction) : localNormal;
        float meanDistance = texture(
            ddgiDistanceMoments,
            vec3(ddgiSampleOctEncode(direction), float(index))).r;
        float normalizedDistance = clamp(
            meanDistance /
                max(ddgiSampling.probeSpacingMaxDistance.w, 1.0e-5),
            0.0, 1.0);
        return vec3(normalizedDistance);
    }
    return state.offsetClassification.w >= 0.5
               ? vec3(0.1, 1.0, 0.2)
               : vec3(1.0, 0.1, 0.1);
}

vec3 evaluateDdgiDiffuse(vec3 positionWS, vec3 normalWS, vec3 viewWS,
                         vec3 albedo, float metallic, float occlusion)
{
    if (!ddgiIsActive()) return vec3(0.0);
    vec3 biasedPosition = positionWS + normalWS *
        ddgiSampling.updateParameters.y + viewWS *
        ddgiSampling.updateParameters.z;
    vec3 localPosition = (ddgiSampling.worldToLocal *
                          vec4(biasedPosition, 1.0)).xyz;
    vec3 center = (vec3(ddgiSampling.probeCounts.xyz) - 1.0) * 0.5;
    vec3 grid = localPosition /
                ddgiSampling.probeSpacingMaxDistance.xyz + center;
    ivec3 base = ivec3(floor(grid));
    vec3 blend = fract(grid);
    vec3 localNormal = normalize(mat3(ddgiSampling.worldToLocal) * normalWS);
    vec3 irradiance = vec3(0.0);
    float weightSum = 0.0;
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                ivec3 coord = base + ivec3(x, y, z);
                if (any(lessThan(coord, ivec3(0))) ||
                    any(greaterThanEqual(coord,
                                         ivec3(ddgiSampling.probeCounts.xyz))))
                    continue;
                uint index = ddgiSampleProbeIndex(coord);
                DdgiProbeStateSample state = ddgiProbeStates.states[index];
                if (state.offsetClassification.w < 0.5) continue;
                vec3 probeLocal = ddgiSampleProbeLocalPosition(
                    coord, state.offsetClassification.xyz);
                vec3 toPoint = localPosition - probeLocal;
                float distanceToProbe = length(toPoint);
                vec3 direction = distanceToProbe > 1.0e-5
                                     ? toPoint / distanceToProbe
                                     : localNormal;
                vec2 moments = texture(ddgiDistanceMoments,
                    vec3(ddgiSampleOctEncode(direction), float(index))).rg;
                float visibility = 1.0;
                if (distanceToProbe > moments.x) {
                    float variance = max(moments.y - moments.x * moments.x,
                                         1.0e-4);
                    float delta = distanceToProbe - moments.x;
                    visibility = variance / (variance + delta * delta);
                    visibility = visibility * visibility * visibility;
                }
                vec3 probeWorld = (ddgiSampling.localToWorld *
                                   vec4(probeLocal, 1.0)).xyz;
                float normalWeight = max(dot(normalWS,
                    normalize(probeWorld - positionWS)), 0.05);
                vec3 axisWeight = mix(1.0 - blend, blend, vec3(x, y, z));
                float weight = axisWeight.x * axisWeight.y * axisWeight.z *
                               visibility * normalWeight * normalWeight;
                vec3 sampleValue = texture(ddgiIrradiance,
                    vec3(ddgiSampleOctEncode(localNormal), float(index))).rgb;
                irradiance += sampleValue * weight;
                weightSum += weight;
            }
        }
    }
    irradiance /= max(weightSum, 1.0e-5);
    return irradiance * albedo * (1.0 - metallic) * occlusion *
           ddgiSampling.updateParameters.w;
}

#endif
