// Flashlight shadow pass (Vulkan, needs NVR DXVK): ShadowMapPS of Effects/Flashlight.fx.hlsl. Binding 2 = depth, binding 4 = the spot
// light's shadow map (red = distance over radius). 1 lit, 0 in shadow, 1 outside the reach and the light's frame.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"




void main() {
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r * cb.cameraData.y;
    vec3 pos = surfaceWorld(uv, depth);
    float radius = max(cb.spotLightPosition.w, 1e-4);
    float dist = length(cb.spotLightPosition.xyz - pos) / radius;
    float lit = 1.0;
    if (dist < 1.0) {
        vec4 clip = lightClip(pos);
        vec2 ndc = clip.xy / max(clip.w, 1e-6);
        if (all(lessThanEqual(abs(ndc), vec2(1.0)))) {
            float stored = textureLod(sampler2D(t_shadow, NVR_SAMPLER(pc.linearSampler)), clipToUv(clip), 0.0).r;
            lit = stored >= dist * 0.95 ? 1.0 : 0.0;
        }
    }
    o_color = vec4(lit, lit, lit, 1.0);
}
