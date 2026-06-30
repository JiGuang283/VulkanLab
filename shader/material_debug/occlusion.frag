#version 450

layout(location = 1) in vec2 fragTexCoord;
layout(location = 4) in vec2 fragTexCoord1;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 3) uniform sampler2D occlusionTexture;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(push.reserved.x - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < push.roughnessAlpha.y) discard;
}

vec2 occlusionTexCoord()
{
    return abs(push.roughnessAlpha.w - 1.0) < 0.5 ? fragTexCoord1
                                                  : fragTexCoord;
}

float materialOcclusion()
{
    float sampled = texture(occlusionTexture, occlusionTexCoord()).r;
    float strength = clamp(push.roughnessAlpha.z, 0.0, 1.0);
    return mix(1.0, sampled, strength);
}

void main()
{
    float baseAlpha = texture(baseColorTexture, fragTexCoord).a *
                      push.baseColorFactor.a;
    applyAlphaCutoff(baseAlpha);

    float occlusion = materialOcclusion();
    outColor = vec4(vec3(occlusion), 1.0);
}
