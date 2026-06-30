#version 450

layout(location = 1) in vec2 fragTexCoord;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    float alpha = texture(baseColorTexture, fragTexCoord).a *
                  push.baseColorFactor.a;
    outColor = vec4(vec3(clamp(alpha, 0.0, 1.0)), 1.0);
}
