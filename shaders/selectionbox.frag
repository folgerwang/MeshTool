#version 450
layout(push_constant) uniform PushConstants {
    mat4 viewProj; mat4 model; vec4 color; vec4 screen; ivec4 textureIndex;
} pc;
layout(location=0) out vec4 outColor;
void main() { outColor = pc.color; }
