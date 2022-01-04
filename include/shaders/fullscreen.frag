#version 400
precision highp float;

in lowp vec2 vTexcoord;
uniform sampler2D tex;

layout(location = 0) out vec4 diffuseColor;

void main(void)
{
    diffuseColor = texture(tex, vTexcoord);
}
