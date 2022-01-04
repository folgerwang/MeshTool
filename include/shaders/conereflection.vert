#version 400
in highp vec3 aPosition;

uniform mediump mat4 uModelMatrix;
uniform mediump mat4 uViewProjMatrix;

out mediump vec2 vTexcoord;
out highp vec3 vPositionWS;

void main(void)
{
	vPositionWS = (uModelMatrix * vec4(aPosition.x, aPosition.y, aPosition.z, 1.0)).xyz;
	gl_Position = uViewProjMatrix * vec4(vPositionWS, 1.0);
	vTexcoord = vec2(aPosition.x * 0.5 + 0.5, aPosition.y * 0.5 + 0.5);
}
