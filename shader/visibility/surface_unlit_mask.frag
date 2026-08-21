#version 450

#extension GL_GOOGLE_include_directive : require
#define SURFACE_ALPHA_MASKED 1
#define VKL_SURFACE_SHADING_MODEL VKL_SHADING_MODEL_UNLIT
#define VKL_SURFACE_RECEIVES_SCREEN_AO 0
#define VKL_SURFACE_NORMAL_MAPPING 0
#include "visibility/surface_common.glsl"
