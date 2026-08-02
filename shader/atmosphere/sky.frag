#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/atmosphere.glsl"

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 ndc = fragUv * 2.0 - 1.0;
    vec4 world = ubo.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 positionWS = world.xyz / max(abs(world.w), 1e-6);
    vec3 direction = normalize(positionWS - ubo.cameraPosWS.xyz);
    vec3 sunDirection = normalize(atmosphere.sunDirectionAngularRadius.xyz);
    vec3 sunRadiance = atmosphere.sunColorIntensity.rgb *
                       atmosphere.sunColorIntensity.a;
    vec3 color = texture(atmosphereSkyView,
                         atmosphereDirectionUv(direction)).rgb * sunRadiance;

    float angularRadius = atmosphere.sunDirectionAngularRadius.w;
    float disk = smoothstep(cos(angularRadius * 1.35), cos(angularRadius),
                            dot(direction, sunDirection));
    vec3 sunTransmittance =
        sampleAtmosphereTransmittance(vec3(0.0), sunDirection);
    color += disk * sunRadiance * sunTransmittance * 12.0;
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
