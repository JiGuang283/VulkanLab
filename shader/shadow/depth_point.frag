#version 450

layout(set = 0, binding = 0) uniform PunctualShadowUniform {
    mat4 viewProjection;
    vec4 lightPositionFar;
} punctualShadow;

layout(location = 2) in vec3 fragWorldPosition;

void main()
{
    float farPlane = max(punctualShadow.lightPositionFar.w, 0.0001);
    gl_FragDepth = clamp(
        length(fragWorldPosition - punctualShadow.lightPositionFar.xyz) /
            farPlane,
        0.0, 1.0);
}
