#version 450

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;
};

layout(location = 0) out vec4 finalColor;

void main(void)
{
    finalColor = uBoxColor;
}
