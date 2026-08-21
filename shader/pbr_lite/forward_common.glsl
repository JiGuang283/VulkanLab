#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_surface.glsl"
#include "include/screen_space_lighting.glsl"
#include "include/pbr_surface_lighting.glsl"

#ifndef VKL_FORWARD_NORMAL_MAPPING
#define VKL_FORWARD_NORMAL_MAPPING 0
#endif
#ifndef VKL_FORWARD_SPECULAR_OUTPUT
#define VKL_FORWARD_SPECULAR_OUTPUT 1
#endif
#ifndef VKL_FORWARD_DIFFUSE_OUTPUT
#define VKL_FORWARD_DIFFUSE_OUTPUT 1
#endif

layout(location = 0) in vec3 fragPositionWS;
layout(location = 1) in vec3 fragNormalWS;
layout(location = 2) in vec2 fragTexCoord;
#if VKL_FORWARD_NORMAL_MAPPING
layout(location = 3) in vec4 fragTangentWS;
#endif
layout(location = 4) in vec2 fragTexCoord1;
layout(location = 5) in vec4 fragColor;

layout(location = 0) out vec4 outColor;
#if VKL_FORWARD_SPECULAR_OUTPUT
layout(location = 1) out vec4 outBaselineSpecular;
#endif
#if VKL_FORWARD_DIFFUSE_OUTPUT
layout(location = 2) out vec4 outBaselineDiffuse;
#endif

void main()
{
    EvaluatedMaterialSurface surface = vklEvaluateMaterialSurface(
        fragTexCoord, fragTexCoord1, fragColor);
    if (vklMaterialAlphaRejected(surface))
        discard;

#if VKL_FORWARD_NORMAL_MAPPING
    vec3 normal = vklMappedNormal(
        fragNormalWS, fragTangentWS, fragTexCoord, gl_FrontFacing);
#else
    vec3 normal = vklGeometricNormal(fragNormalWS, gl_FrontFacing);
#endif
    float screenAo =
        (!vklMaterialIsBlend(surface) && surface.transmission <= 0.0)
            ? screenSpaceAmbientOcclusion()
            : 1.0;
    PbrSurfaceLightingInput lightingInput;
    lightingInput.positionWS = fragPositionWS;
    lightingInput.normalWS = normal;
    lightingInput.baseColor = surface.baseColor.rgb;
    lightingInput.roughness = surface.roughness;
    lightingInput.metallic = surface.metallic;
    lightingInput.materialOcclusion = surface.materialOcclusion;
    lightingInput.emissive = surface.emissive;
    lightingInput.receivesScreenAo =
        !vklMaterialIsBlend(surface) && surface.transmission <= 0.0 ? 1u : 0u;
    PbrSurfaceLightingResult lighting = vklEvaluatePbrSurfaceLighting(
        lightingInput, screenAo);
    if (lighting.debugOverride == 0u) {
        vec3 view = normalize(ubo.cameraPosWS.xyz - fragPositionWS);
        lighting.color = vklApplyTransmissionApprox(
            surface, lighting.color, normal, view);
    }
    vklApplyPbrAerialPerspective(lighting, fragPositionWS);
    outColor = vec4(lighting.color, vklMaterialOutputAlpha(surface));
#if VKL_FORWARD_SPECULAR_OUTPUT
    outBaselineSpecular = vec4(lighting.baselineSpecular, 1.0);
#endif
#if VKL_FORWARD_DIFFUSE_OUTPUT
    outBaselineDiffuse = vec4(lighting.baselineDiffuse,
                              surface.materialOcclusion);
#endif
}
