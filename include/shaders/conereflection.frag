#version 400
precision highp float;

in mediump vec2 vTexcoord;
in highp vec3 vPositionWS;
uniform mediump mat4 uConeViewProjMatrix;
uniform highp vec4 uEyePosition;

uniform sampler2D colorTex;
uniform sampler2D depthTex;
uniform lowp vec4 uBoxColor;

layout(location = 0) out vec4 diffuseColor;

void main(void)
{
	vec3 lightDir = vec3(-0.1f, -0.6f, 0.4f);
	vec3 normalWS = normalize(cross(dFdx(vPositionWS), dFdy(vPositionWS)));
	vec3 viewWS = normalize(uEyePosition.xyz - vPositionWS);
	vec3 reflectWS = 2.0f * dot(viewWS, normalWS) * normalWS - viewWS;
	float diffuse = max(dot(-lightDir, normalWS), 0.0f);
	vec4 positionCSS = uConeViewProjMatrix * vec4(vPositionWS + reflectWS / dot(viewWS, normalWS), 1.0f);
	positionCSS.xyz /= positionCSS.w;
 	diffuseColor = uBoxColor * (diffuse * 0.8f + 0.2f);
	if (positionCSS.x > -1.0f && positionCSS.x < 1.0f && positionCSS.y > -1.0f && positionCSS.y < 1.0f && positionCSS.w > 0)
		diffuseColor = texture(colorTex, (positionCSS.xy * vec2(0.5f, 0.5f) + 0.5f));
}
