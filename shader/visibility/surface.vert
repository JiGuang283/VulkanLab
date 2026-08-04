#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_push.glsl"
#include "include/surface_frame.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec3 fragNormalWS;
layout(location = 1) out vec4 fragTangentWS;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragColor;
layout(location = 4) out vec4 fragCurrentClip;
layout(location = 5) out vec4 fragPreviousClip;
layout(location = 6) flat out uint fragHistoryValid;

void main()
{
    RenderItemHistory history = surfaceHistory.items[gl_InstanceIndex];
    vec4 positionWS = push.model * vec4(inPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(push.model)));
    vec3 normalWS = normalize(normalMatrix * inNormal);
    vec3 tangentWS = normalize(mat3(push.model) * inTangent.xyz);
    tangentWS = normalize(tangentWS - normalWS * dot(normalWS, tangentWS));

    fragCurrentClip = ubo.proj * ubo.view * positionWS;
    fragPreviousClip = surfaceFrame.previousViewProjection *
                       history.previousWorld * vec4(inPosition, 1.0);
    fragNormalWS = normalWS;
    fragTangentWS = vec4(tangentWS, inTangent.w);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragHistoryValid = surfaceFrame.params.x != 0u &&
                       history.params.x != 0u ? 1u : 0u;
    gl_Position = fragCurrentClip;
}
