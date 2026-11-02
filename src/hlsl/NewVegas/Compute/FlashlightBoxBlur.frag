// Flashlight box blurs (Vulkan, needs NVR DXVK): MinBlurPS and AvgBlurPS of Effects/Flashlight.fx.hlsl. Five taps (the centre and the four
// diagonal neighbours half a texel away) of the chain's current image (binding 1). mode 0: the minimum; mode 1: the mean (RGB, alpha 1).
// The scale is 1 (no render-scale rect: this pass is off in rect frames).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"




vec3 tap(vec2 p) {
    return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), p, 0.0).rgb;
}

void main() {
    vec2 h = 0.5 * cb.reciprocalResolution.xy;
    vec3 a = tap(uv), b = tap(uv + vec2(h.x, h.y)), c = tap(uv + vec2(-h.x, h.y)), d = tap(uv + vec2(h.x, -h.y)), e = tap(uv + vec2(-h.x, -h.y));
    vec3 r;
    if (pc.mode == 0u) {
        r = min(min(min(a, b), min(c, d)), e);
    } else {
        r = (a + b + c + d + e) * 0.2;
    }
    o_color = vec4(r, 1.0);
}
