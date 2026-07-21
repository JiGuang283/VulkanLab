#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

void main()
{
    float alpha = texture(baseColorTexture, fragTexCoord).a *
                  push.baseColorFactor.a * fragColor.a;
    if (alpha < push.roughnessAlpha.y)
        discard;
}
