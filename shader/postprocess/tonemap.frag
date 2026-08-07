#version 450

layout(location = 0) in vec2 fragUv;
layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D bloomColor;
layout(set = 0, binding = 2) uniform sampler2D surfaceNormalRoughness;
layout(set = 0, binding = 3) uniform sampler2D surfaceMotion;
layout(set = 0, binding = 4) uniform sampler2D screenDebugSource;

layout(push_constant) uniform ToneMapPushConstants {
    float exposureEv;
    float bloomIntensity;
    uint toneMapper;
    uint encodeGamma;
    uint applyExposure;
    uint applyBloom;
    uint surfaceDebugMode;
    float motionDebugScale;
    uint screenDebugMode;
    uint screenDebugMip;
    float cameraNear;
    float cameraFar;
} push;

layout(location = 0) out vec4 outColor;

vec3 acesFitted(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) /
                 (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 octDecode(vec2 encoded)
{
    vec2 value = encoded * 2.0 - 1.0;
    vec3 normal = vec3(value, 1.0 - abs(value.x) - abs(value.y));
    if (normal.z < 0.0)
        normal.xy = (1.0 - abs(normal.yx)) * sign(normal.xy);
    return normalize(normal);
}

vec3 applyDisplayTransform(vec3 color, bool allowToneMap)
{
    color = max(color, vec3(0.0));
    if (allowToneMap && push.applyExposure != 0u)
        color *= exp2(push.exposureEv);
    if (allowToneMap && push.toneMapper == 1u)
        color = color / (vec3(1.0) + color);
    else if (allowToneMap && push.toneMapper == 2u)
        color = acesFitted(color);
    if (push.encodeGamma != 0u)
        color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    return color;
}

void main()
{
    if (push.surfaceDebugMode != 0u) {
        vec4 surface = texture(surfaceNormalRoughness, fragUv);
        vec3 debugColor = vec3(0.0);
        if (push.surfaceDebugMode == 1u)
            debugColor = octDecode(surface.rg) * 0.5 + 0.5;
        else if (push.surfaceDebugMode == 2u)
            debugColor = vec3(surface.b);
        else if (push.surfaceDebugMode == 3u)
            debugColor = vec3(texture(surfaceMotion, fragUv).rg *
                                  push.motionDebugScale +
                              vec2(0.5),
                              0.5);
        else if (push.surfaceDebugMode == 4u)
            debugColor = vec3(surface.a);
        if (push.encodeGamma != 0u)
            debugColor = pow(max(debugColor, vec3(0.0)), vec3(1.0 / 2.2));
        outColor = vec4(debugColor, 1.0);
        return;
    }

    if (push.screenDebugMode != 0u) {
        vec3 debugColor = vec3(0.0);
        bool allowToneMap = false;
        if (push.screenDebugMode == 1u) {
            int levels = textureQueryLevels(screenDebugSource);
            float mip = float(min(push.screenDebugMip,
                                  uint(max(levels - 1, 0))));
            float depth = textureLod(screenDebugSource, fragUv, mip).r;
            float nearPlane = max(push.cameraNear, 1e-4);
            float farPlane = max(push.cameraFar, nearPlane + 1e-3);
            float linearDepth = nearPlane * farPlane /
                max(farPlane - depth * (farPlane - nearPlane), 1e-4);
            float normalized = log2(max(linearDepth / nearPlane, 1.0)) /
                max(log2(farPlane / nearPlane), 1e-4);
            debugColor = vec3(clamp(normalized, 0.0, 1.0));
        } else if (push.screenDebugMode == 2u) {
            int levels = textureQueryLevels(screenDebugSource);
            float mip = float(min(push.screenDebugMip,
                                  uint(max(levels - 1, 0))));
            debugColor = textureLod(screenDebugSource, fragUv, mip).rgb;
            allowToneMap = true;
        } else if (push.screenDebugMode == 3u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 4u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 5u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 6u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 7u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 8u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 9u ||
                   push.screenDebugMode == 10u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 11u) {
            debugColor = texture(screenDebugSource, fragUv).rgb;
            allowToneMap = true;
        } else if (push.screenDebugMode == 12u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        } else if (push.screenDebugMode == 13u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).g);
        } else if (push.screenDebugMode == 14u ||
                   push.screenDebugMode == 15u ||
                   push.screenDebugMode == 16u) {
            debugColor = texture(screenDebugSource, fragUv).rgb;
            allowToneMap = true;
        } else if (push.screenDebugMode == 17u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).g);
        } else if (push.screenDebugMode == 18u) {
            debugColor = vec3(texture(screenDebugSource, fragUv).r);
        }
        outColor = vec4(applyDisplayTransform(debugColor, allowToneMap), 1.0);
        return;
    }

    vec3 color = max(texture(hdrColor, fragUv).rgb, vec3(0.0));
    if (push.applyBloom != 0u) {
        color += max(texture(bloomColor, fragUv).rgb, vec3(0.0)) *
                 push.bloomIntensity;
    }
    outColor = vec4(applyDisplayTransform(color, true), 1.0);
}
