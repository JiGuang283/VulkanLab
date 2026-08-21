#version 450

#extension GL_GOOGLE_include_directive : require
#define VKL_GBUFFER_ALPHA_MASKED 0
#define VKL_GBUFFER_SHADING_MODEL VKL_SHADING_MODEL_UNLIT
#define VKL_GBUFFER_RECEIVES_SCREEN_AO 0
#define VKL_GBUFFER_NORMAL_MAPPING 0
#include "deferred/gbuffer_common.glsl"
