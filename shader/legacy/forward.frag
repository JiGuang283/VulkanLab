#version 450

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;

layout(set = 1, binding = 0) uniform sampler2D texSampler;

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
    vec4 tex = texture(texSampler, fragTexCoord);
    vec4 albedo = tex * push.baseColorFactor;
    applyAlphaCutoff(albedo.a);

    vec3 n = normalize(fragNormalWS);
    if (!gl_FrontFacing)
        n = -n;
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    vec3 lit = albedo.rgb * (0.25 + 0.75 * ndl) + push.emissiveMetallic.rgb;
    outColor = vec4(lit, albedo.a);
}
