#version 450

layout(location = 0) in vec2 aPosition;

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;
};

layout(location = 0) out vec2 vTextureCoord;

void main(void)
{
    vTextureCoord = aPosition.xy;
    gl_Position = vec4(uScreenPosition.xy + (aPosition.xy * 2.0 - 1.0) * uScreenPosition.zw, 0.0, 1.0);
}
