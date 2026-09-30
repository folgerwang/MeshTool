#version 450

layout(location = 0) in vec3 aPosition;

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;
};

void main(void)
{
    vec3 positionWS = (uModelMatrix * vec4(aPosition, 1.0)).xyz;
    gl_Position = uViewProjMatrix * vec4(positionWS, 1.0);
    gl_Position.z -= gl_Position.w * 0.00001;
}
