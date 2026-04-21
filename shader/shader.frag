# version 450

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;

layout(binding = 1) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;   // xyz=emissive, w=metallic
    vec4 roughnessAlpha;     // x=roughness, y=alphaCutoff
    vec4 reserved;
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 tex = texture(texSampler, fragTexCoord);
    vec4 albedo = tex * push.baseColorFactor;
    if (albedo.a < push.roughnessAlpha.y) discard;

    vec3 n = normalize(fragNormalWS);
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    vec3 lit = albedo.rgb * (0.25 + 0.75 * ndl) + push.emissiveMetallic.rgb;
    outColor = vec4(lit, albedo.a);
}