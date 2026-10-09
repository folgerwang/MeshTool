#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Glass facades: the captured colour, made translucent and reflective.
// Positions are camera-relative, so the view direction is -vPositionWS.
// The scene is Z-up.

layout(location = 0) in vec3 vPositionWS;
layout(location = 1) in vec2 vTextureCoord;

layout(push_constant) uniform PushConstants {
    mat4 uViewProjMatrix;
    mat4 uModelMatrix;
    vec4 uBoxColor;
    vec4 uScreenPosition;   // y = opacity facing the viewer, z = reflectivity
    ivec4 uTexture;         // x = slot in uTextures
};

layout(set = 0, binding = 0) uniform sampler2D uTextures[];   // bindless

layout(location = 0) out vec4 finalColor;

const vec3 kSunDir = normalize(vec3(0.35, -0.45, 0.82));

vec3 Sky(vec3 r)
{
    // Ground below the horizon, a pale horizon and a deeper zenith above it.
    vec3 ground = vec3(0.23, 0.24, 0.25);
    vec3 horizon = vec3(0.78, 0.84, 0.90);
    vec3 zenith = vec3(0.33, 0.52, 0.80);
    if (r.z < 0.0)
        return mix(horizon * 0.6, ground, smoothstep(0.0, 0.25, -r.z));
    return mix(horizon, zenith, pow(smoothstep(0.0, 1.0, r.z), 0.6));
}

void main(void)
{
    vec3 V = normalize(-vPositionWS);
    vec3 N = normalize(cross(dFdx(vPositionWS), dFdy(vPositionWS)));
    if (dot(N, V) < 0.0)
        N = -N;                                    // glass is seen from both sides

    vec3 base = uBoxColor.w > 0.5 ? texture(uTextures[uTexture.x], vTextureCoord).xyz : uBoxColor.xyz;
    float opacity = uScreenPosition.y;
    float reflectivity = uScreenPosition.z;

    // A curtain wall is panes in a frame: the captured panes are the darker
    // texels, mullions and spandrels the brighter ones, which stay opaque so
    // the facade keeps its captured grid.
    float lum = dot(base, vec3(0.299, 0.587, 0.114));
    float pane = 1.0 - smoothstep(0.42, 0.62, lum);

    // Schlick Fresnel for glass (F0 = 0.04): weak head-on, strong at grazing angles.
    float cosT = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = 0.04 + 0.96 * pow(1.0 - cosT, 5.0);
    float k = clamp(reflectivity * (0.35 + 0.65 * fresnel), 0.0, 1.0) * pane;

    vec3 R = reflect(-V, N);
    vec3 color = mix(base, Sky(R), k);
    float sun = pow(max(dot(R, kSunDir), 0.0), 600.0) * 4.0 * reflectivity * pane;
    color += vec3(1.0, 0.97, 0.9) * sun;

    // Panes let the dark interior through, more opaque where they reflect
    // more, so grazing glass reads as a mirror.
    float alpha = mix(1.0, clamp(mix(opacity, 1.0, fresnel) + sun, 0.0, 1.0), pane);
    finalColor = vec4(color, alpha);
}
