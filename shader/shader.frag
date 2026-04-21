# version 450

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 tex = texture(texSampler, fragTexCoord);
    vec3 n = normalize(fragNormalWS);
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    outColor = vec4(tex.rgb * (0.25 + 0.75 * ndl), tex.a);
}