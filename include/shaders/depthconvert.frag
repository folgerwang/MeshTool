#version 400
precision highp float;

in lowp vec2 vTexcoord;
uniform sampler2D depthTex;

layout(location = 0) out float diffuseColor;

void main(void)
{
    diffuseColor = texture(depthTex, vTexcoord).x;
}
