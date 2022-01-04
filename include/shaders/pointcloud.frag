#version 400
precision highp float;

in lowp vec3 vColor;

layout(location = 0) out vec4 diffuseColor;

void main(void)
{
    diffuseColor = vec4(vColor, 1.0f);
}
