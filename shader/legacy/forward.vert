#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_push.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 4) in vec2 inTexCoord1;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec3 fragNormalWS;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 5) out vec4 fragColor;

void main(){
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    fragNormalWS = normalize(transpose(inverse(mat3(push.model))) * inNormal);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
}
