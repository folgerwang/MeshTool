#version 450

layout(location = 0) in vec2 vTextureCoord;

layout(set = 0, binding = 0) uniform sampler2D colorTex;

layout(location = 0) out vec4 finalColor;

void main(void)
{
    vec3 srcColor = texture(colorTex, vTextureCoord).xyz;
    float alpha = min(dot(srcColor, vec3(2.0, 2.0, 2.0)), 1.0);
    finalColor = vec4(srcColor, alpha);
}
