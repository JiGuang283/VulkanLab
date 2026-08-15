#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_push.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main()
{
    uint cascadeIndex = push.indices.z;
    gl_Position = ubo.cascadeViewProj[cascadeIndex] * push.model *
                  vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
}
