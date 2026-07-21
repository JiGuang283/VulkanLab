#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"

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
