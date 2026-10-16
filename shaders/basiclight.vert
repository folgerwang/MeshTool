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
    // LOD depth order: uScreenPosition.x pushes the mesh back along the view
    // rays (same pixels, later depth; 0 = off). Positions are camera-relative,
    // so under perspective the push is a scale that moves each vertex the
    // given number of metres farther along its ray; under the orthographic
    // projection (w = 1) scaling would shift pixels sideways, so the push,
    // given in depth units, goes into the depth directly (reversed depth:
    // farther is smaller).
    float push = max(uScreenPosition.x, 0.0);
    if (uScreenPosition.w > 0.5)
    {
        gl_Position = uViewProjMatrix * vec4(vPositionWS, 1.0);
        gl_Position.z -= push * gl_Position.w;
    }
    else
    {
        float dist = max(length(vPositionWS), 1e-3);
        gl_Position = uViewProjMatrix * vec4(vPositionWS * (1.0 + push / dist), 1.0);
    }
}
