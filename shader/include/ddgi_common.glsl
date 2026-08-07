#ifndef VKL_DDGI_COMMON_GLSL
#define VKL_DDGI_COMMON_GLSL

struct GpuDdgiProbeState {
    vec4 offsetClassification;
    vec4 statistics;
};

layout(std140, set = 1, binding = 6) uniform DdgiParameters {
    mat4 localToWorld;
    mat4 worldToLocal;
    uvec4 probeCounts;
    vec4 probeSpacingMaxDistance;
    vec4 updateParameters;
    uvec4 runtimeParameters;
    uvec4 updateWindow;
    vec4 traceParameters;
} ddgi;

uint ddgiProbeIndex(uvec3 coord)
{
    return coord.x + ddgi.probeCounts.x *
           (coord.y + ddgi.probeCounts.y * coord.z);
}

uvec3 ddgiProbeCoord(uint index)
{
    uint xy = ddgi.probeCounts.x * ddgi.probeCounts.y;
    uint z = index / xy;
    uint rem = index - z * xy;
    uint y = rem / ddgi.probeCounts.x;
    return uvec3(rem - y * ddgi.probeCounts.x, y, z);
}

vec3 ddgiProbeLocalPosition(uvec3 coord, vec3 offset)
{
    vec3 center = (vec3(ddgi.probeCounts.xyz) - 1.0) * 0.5;
    return (vec3(coord) - center) * ddgi.probeSpacingMaxDistance.xyz +
           offset;
}

vec2 ddgiOctEncode(vec3 direction)
{
    direction /= max(abs(direction.x) + abs(direction.y) +
                     abs(direction.z), 1.0e-6);
    vec2 encoded = direction.xy;
    if (direction.z < 0.0)
        encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return encoded * 0.5 + 0.5;
}

vec3 ddgiOctDecode(vec2 encoded)
{
    vec2 f = encoded * 2.0 - 1.0;
    vec3 direction = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (direction.z < 0.0)
        direction.xy = (1.0 - abs(direction.yx)) * sign(direction.xy);
    return normalize(direction);
}

#endif
