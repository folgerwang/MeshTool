#version 450
layout(push_constant) uniform PushConstants {
    mat4 viewProj; mat4 model; vec4 color; vec4 screen; ivec4 textureIndex;
} pc;
const int corners[24] = int[](0,1,2,3,4,5,6,7,0,2,1,3,4,6,5,7,0,4,1,5,2,6,3,7);
void main() {
    int c = corners[gl_VertexIndex];
    vec3 p = vec3(float(c & 1), float((c >> 1) & 1), float((c >> 2) & 1));
    gl_Position = pc.viewProj * pc.model * vec4(p, 1.0);
    // Small forward offset keeps surface-coincident edges stable.
    gl_Position.z -= gl_Position.w * 0.00001;
}
