#version 400
in highp vec2 aPosition;

out lowp vec2 vTexcoord;

void main(void)
{
    gl_Position = vec4(aPosition.x, aPosition.y, 0.0, 1.0);
    vTexcoord = vec2(aPosition.x * 0.5 + 0.5, aPosition.y * 0.5 + 0.5);
}
