#ifndef VKL_SURFACE_ENCODING_GLSL
#define VKL_SURFACE_ENCODING_GLSL

const uint VKL_SURFACE_FLAG_HISTORY_VALID_BIT = 1u << 0u;
const uint VKL_SURFACE_FLAG_SHADING_MODEL_SHIFT = 1u;
const uint VKL_SURFACE_FLAG_SHADING_MODEL_MASK = 0x7u << 1u;
const uint VKL_SURFACE_FLAG_RECEIVES_SCREEN_AO_BIT = 1u << 4u;

const uint VKL_SHADING_MODEL_DEFAULT_LIT = 0u;
const uint VKL_SHADING_MODEL_UNLIT = 1u;

uint vklPackSurfaceFlags(uint shadingModel, bool historyValid,
                         bool receivesScreenAo)
{
    return (historyValid ? VKL_SURFACE_FLAG_HISTORY_VALID_BIT : 0u) |
           ((shadingModel << VKL_SURFACE_FLAG_SHADING_MODEL_SHIFT) &
            VKL_SURFACE_FLAG_SHADING_MODEL_MASK) |
           (receivesScreenAo ? VKL_SURFACE_FLAG_RECEIVES_SCREEN_AO_BIT : 0u);
}

uint vklDecodeSurfaceFlags(float encodedFlags)
{
    return uint(round(max(encodedFlags, 0.0)));
}

uint vklDecodeShadingModel(uint surfaceFlags)
{
    return (surfaceFlags & VKL_SURFACE_FLAG_SHADING_MODEL_MASK) >>
           VKL_SURFACE_FLAG_SHADING_MODEL_SHIFT;
}

vec2 vklOctEncode(vec3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    vec2 encoded = normal.xy;
    if (normal.z < 0.0)
        encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return encoded * 0.5 + 0.5;
}

vec3 vklOctDecode(vec2 encoded)
{
    vec2 octahedron = encoded * 2.0 - 1.0;
    vec3 normal = vec3(octahedron,
                       1.0 - abs(octahedron.x) - abs(octahedron.y));
    if (normal.z < 0.0)
        normal.xy = (1.0 - abs(normal.yx)) * sign(normal.xy);
    return normalize(normal);
}

#endif
