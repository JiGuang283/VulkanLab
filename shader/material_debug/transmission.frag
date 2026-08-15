#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    float transmission = clamp(materialData().transmissionVolume.x, 0.0, 1.0);
    outColor = vec4(vec3(transmission), 1.0);
}
