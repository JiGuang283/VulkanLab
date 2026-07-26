#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"

layout(location = 0) in vec2 fragUv;
layout(set = 2, binding = 4) uniform samplerCube radianceMap;
layout(location = 0) out vec4 outColor;

vec3 rotateEnvironmentDirection(vec3 direction)
{
    float angle = -ubo.environmentParams.z;
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * direction.x - s * direction.y,
                s * direction.x + c * direction.y,
                direction.z);
}

void main()
{
    vec2 ndc = fragUv * 2.0 - 1.0;
    vec4 world = ubo.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 positionWS = world.xyz / max(abs(world.w), 1e-6);
    vec3 direction =
        normalize(positionWS - ubo.cameraPosWS.xyz);
    vec3 color = textureLod(
        radianceMap, rotateEnvironmentDirection(direction), 0.0).rgb;
    outColor = vec4(color * ubo.environmentParams.y, 1.0);
}
