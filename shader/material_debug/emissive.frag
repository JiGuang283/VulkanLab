#version 450

layout(location = 1) in vec2 fragTexCoord;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 4) uniform sampler2D emissiveTexture;

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

void main()
{
    float baseAlpha = texture(baseColorTexture, fragTexCoord).a *
                      push.baseColorFactor.a;
    applyAlphaCutoff(baseAlpha);

    vec3 emissive = texture(emissiveTexture, fragTexCoord).rgb *
                    push.emissiveMetallic.rgb;
    outColor = vec4(emissive, 1.0);
}
