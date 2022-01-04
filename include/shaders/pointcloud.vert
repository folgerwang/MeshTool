#version 400
in highp vec3 aPosition;
in lowp vec4 aColor;

uniform mediump mat4 uModelMatrix;
uniform mediump mat4 uViewProjMatrix;

out lowp vec3 vColor;

void main(void)
{
	vec3 positionWS = (uModelMatrix * vec4(aPosition.x, aPosition.y, aPosition.z, 1.0)).xyz;
	gl_Position = uViewProjMatrix * vec4(positionWS, 1.0);
	vColor = aColor.xyz;
}
