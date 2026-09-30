#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexcoord;

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;
};

layout(location = 0) out vec3 vPositionWS;
layout(location = 1) out vec2 vTextureCoord;

void main(void)
{
    vPositionWS = (uModelMatrix * vec4(aPosition, 1.0)).xyz;
    vTextureCoord = aTexcoord;
    gl_Position = uViewProjMatrix * vec4(vPositionWS, 1.0);
}
