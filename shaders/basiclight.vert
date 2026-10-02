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
    // Positions are camera-relative, so scaling them moves the mesh along the
    // view rays only: same pixels, later depth (LOD depth order; 0 = off).
    float depthScale = uScreenPosition.x > 0.0 ? uScreenPosition.x : 1.0;
    gl_Position = uViewProjMatrix * vec4(vPositionWS * depthScale, 1.0);
}
