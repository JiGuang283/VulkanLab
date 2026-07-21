#version 450

const int MAX_DIRECTIONAL_LIGHTS = 1;
const int MAX_PUNCTUAL_LIGHTS = 8;

struct GpuLight {
    vec4 positionRange;
    vec4 directionInnerCos;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 cameraPosWS;
    vec4 ambientColorIntensity;
    vec4 lightCounts;
    GpuLight directionalLights[MAX_DIRECTIONAL_LIGHTS];
    GpuLight punctualLights[MAX_PUNCTUAL_LIGHTS];
    mat4 directionalShadowViewProj;
    vec4 shadowParams;
} ubo;

layout(location = 0) in vec3 fragPositionWS;
layout(set = 2, binding = 0) uniform sampler2DShadow directionalShadowMap;
layout(location = 0) out vec4 outColor;

float shadowVisibility(vec3 positionWS)
{
    if (ubo.shadowParams.x < 0.5)
        return 1.0;
    vec4 clip = ubo.directionalShadowViewProj * vec4(positionWS, 1.0);
    vec3 coord = clip.xyz / clip.w;
    vec2 uv = coord.xy * 0.5 + 0.5;
    if (coord.z <= 0.0 || coord.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * ubo.shadowParams.z;
            visibility += texture(directionalShadowMap,
                                  vec3(uv + offset,
                                       coord.z - ubo.shadowParams.y));
        }
    }
    return visibility / 9.0;
}

void main()
{
    float visibility = shadowVisibility(fragPositionWS);
    outColor = vec4(vec3(visibility), 1.0);
}
