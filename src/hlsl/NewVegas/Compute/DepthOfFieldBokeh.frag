// DepthOfField autofocus blur passes 3 to 5 as a Vulkan pass (needs NVR DXVK): BokehBlur of Effects/DepthOfField.fx.hlsl, line for line.
// The same pipeline for all three runs: push useBokeh 1 takes the brightest valid tap (BaseBlurRadius), 0 the plain average (Radius).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_chain;      // the packed image (the previous pass's output)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    bool bokeh = pc.useBokeh > 0.5;
    vec4 centre = DOF_SAMPLE(t_chain, uv);
    vec2 q = dofQuadOf(uv);
    if (q.y > 0.5) {
        o_color = centre;
        return;
    }
    vec2 l = dofQuadLocal(uv);
    float amount = q.x < 0.5 ? DOF_SAMPLE(t_chain, dofPacked(vec2(0.0, 1.0), l)).r
                             : DOF_SAMPLE(t_chain, dofPacked(vec2(1.0, 1.0), l)).g;
    float radiusBase = bokeh ? pc.blur.w : pc.data.y;
    float radius = radiusBase * amount;
    if (radius <= 0.0) {
        o_color = centre;
        return;
    }
    vec2 texel = pc.reciprocalResolution.xy;
    vec2 lo = q * 0.5 + 0.5 * texel;
    vec2 hi = (q + 1.0) * 0.5 - 0.5 * texel;
    vec4 sum = vec4(0.0);
    float count = 0.0;
    vec4 best = centre;
    float bestLuma = -1.0;
    for (int k = 0; k < 12; k++) {
        float a = float(k) * 0.52359878;
        float r = mod(float(k), 2.0) < 0.5 ? 1.0 : 0.5;
        vec2 t = uv + vec2(cos(a), sin(a)) * (r * radius) * texel;
        float valid = (all(greaterThanEqual(t, lo)) && all(lessThanEqual(t, hi))) ? 1.0 : 0.0;
        vec4 c = DOF_SAMPLE(t_chain, clamp(t, lo, hi));
        sum += c * valid;
        count += valid;
        float lum = dofLuma(c.rgb);
        if (valid > 0.5 && lum > bestLuma) {
            bestLuma = lum;
            best = c;
        }
    }
    if (count < 0.5) {
        o_color = centre;
        return;
    }
    if (bokeh && bestLuma > pc.data.z) {
        o_color = best;
        return;
    }
    o_color = sum / count;
}
