// Flashlight light pass (Vulkan, needs NVR DXVK): LightPS of Effects/Flashlight.fx.hlsl, line for line. Mode 0 reads the shadow term from
// the input (binding 1); mode 1 (no shadows) uses 1. Bindings: 1 = input (shadow term), 2 = depth, 3 = normals, 5 = cookie (mipped sampler).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"




void main() {
    float shadow = pc.mode == 1u ? 1.0 : textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r;
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r * cb.cameraData.y;
    vec3 pos = surfaceWorld(uv, depth);
    vec3 nView = textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).xyz * 2.0 - 1.0;
    vec3 n = (vec4(nView, 0.0) * cb.viewTransform).xyz;	// D3D mul(M, n)
    n = n * inversesqrt(max(dot(n, n), 1e-8));

    vec3 toLight = cb.spotLightPosition.xyz - pos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);
    float ndl = dot(n, L);
    float diffuse = mix(clamp(ndl, 0.0, 1.0), 0.75 * (ndl * 0.5 + 0.5) * (ndl * 0.5 + 0.5), cb.flashLightTuning.y);

    vec3 V = cb.cameraPosition.xyz - pos;
    V = V * inversesqrt(max(dot(V, V), 1e-8));
    vec3 HV = L + V;
    vec3 H = HV * inversesqrt(max(dot(HV, HV), 1e-8));
    float specular = pow(clamp(dot(n, H), 0.0, 1.0), 5.0);

    float nearFade = cb.flashLightTuning.x > 0.0 ? smoothstep(0.0, cb.flashLightTuning.x, dist) : 1.0;

    vec4 clip = lightClip(pos);
    vec2 cuv = clipToUv(clip);
    float cookie = texture(sampler2D(t_cookie, NVR_SAMPLER(pc.cookieSampler)), cuv).r * cookieBreakup(cuv, cb.flashLightTuning.w);

    vec3 light = (diffuse + specular) * lightColour() * coneAt(pos) * falloff(dist) * cookie * shadow * nearFade;
    o_color = vec4(max(light, vec3(0.0)), 1.0);
}
