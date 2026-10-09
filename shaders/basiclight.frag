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
    if (uTexture.y < 3 || uTexture.y > 6)
    {
        finalColor = vec4(srcColor * (diffuse * 0.2 + 0.8), 1.0);
        return;
    }
    // Cook-Torrance GGX with finish-specific roughness/metalness. Captured
    // albedo remains the base colour; derivatives supply geometric normals.
    vec3 N = normalWS;
    vec3 V = normalize(-vPositionWS);
    if (dot(N,V) < 0.0) N = -N;
    vec3 L = normalize(vec3(0.35,-0.45,0.82));
    vec3 H = normalize(V+L);
    float nv = max(dot(N,V),0.001), nl = max(dot(N,L),0.0);
    float nh = max(dot(N,H),0.0), vh = max(dot(V,H),0.0);
    float roughness = uTexture.y == 4 ? 0.30 : uTexture.y == 5 ? 0.92 : 0.80;
    float metallic = uTexture.y == 4 ? 0.85 : 0.0;
    vec3 base = pow(max(srcColor,vec3(0.0)),vec3(2.2));
    vec3 f0 = mix(vec3(0.04),base,metallic);
    vec3 F = f0 + (1.0-f0)*pow(1.0-vh,5.0);
    float a2 = pow(roughness,4.0);
    float d = nh*nh*(a2-1.0)+1.0;
    float D = a2 / max(3.14159265*d*d,0.00001);
    float k = (roughness+1.0)*(roughness+1.0)/8.0;
    float G = nv/(nv*(1.0-k)+k) * nl/max(nl*(1.0-k)+k,0.00001);
    vec3 specular = D*G*F/max(4.0*nv*nl,0.00001);
    vec3 kd = (1.0-F)*(1.0-metallic);
    vec3 direct = (kd*base/3.14159265 + specular)*nl*vec3(2.4,2.3,2.1);
    vec3 R = reflect(-V,N);
    vec3 sky = mix(vec3(0.18,0.19,0.20),vec3(0.45,0.60,0.78),smoothstep(-0.1,0.8,R.z));
    vec3 ambient = base*(1.0-metallic)*0.34 + sky*f0*(1.0-roughness*0.65);
    vec3 lit = direct+ambient;
    finalColor = vec4(pow(lit/(lit+vec3(1.0)),vec3(1.0/2.2)),1.0);
}
