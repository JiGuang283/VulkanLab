#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/shadow_sampling.glsl"

layout(location = 0) in vec3 fragPositionWS;
layout(location = 0) out vec4 outColor;

void main()
{
    float visibility = csmShadowVisibility(fragPositionWS);
    outColor = vec4(vec3(visibility), 1.0);
}
