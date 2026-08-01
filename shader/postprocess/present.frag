#version 450

layout(location = 0) in vec2 fragUv;
layout(set = 0, binding = 0) uniform sampler2D viewportColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(viewportColor, fragUv);
}
