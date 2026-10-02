#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 vTextureCoord;

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;
    ivec4 uTexture;     // x = slot in uTextures
};

layout(set = 0, binding = 0) uniform sampler2D uTextures[];   // bindless

layout(location = 0) out vec4 finalColor;

void main(void)
{
    vec3 srcColor = texture(uTextures[uTexture.x], vTextureCoord).xyz;
    float alpha = min(dot(srcColor, vec3(2.0, 2.0, 2.0)), 1.0);
    finalColor = vec4(srcColor, alpha);
}
