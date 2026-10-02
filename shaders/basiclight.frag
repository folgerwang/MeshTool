#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 vPositionWS;
layout(location = 1) in vec2 vTextureCoord;

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
    vec3 lightDir = vec3(0.1, 0.6, -0.4);
    vec3 normalWS = normalize(cross(dFdx(vPositionWS), dFdy(vPositionWS)));
    float diffuse = max(dot(-lightDir, normalWS), 0.0);

    vec3 srcColor = uBoxColor.w > 0.5 ? texture(uTextures[uTexture.x], vTextureCoord).xyz : uBoxColor.xyz;
    finalColor = vec4(srcColor * (diffuse * 0.2 + 0.8), 1.0);
}
