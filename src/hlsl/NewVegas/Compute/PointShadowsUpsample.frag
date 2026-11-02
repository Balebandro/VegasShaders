// PointShadows technique 4 (LampMaskUpsample) as a Vulkan pass (needs NVR DXVK): the D3D9 UpsamplePS of Effects/PointShadows.fx.hlsl.
// Binding 0 = the half-resolution mask (R mask, G stored depth), read with the point sampler (push byte 4, as the C++ binds it);
// binding 1 = depth (linear, push byte 0).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_half;
layout(set = 1, binding = 1) uniform texture2D t_depth;

layout(push_constant) uniform Push {
    uint linearSampler;   // 0
    uint pointSampler;    // 4 (the sampler for the half mask)
    uint mode;            // 8
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void upTap(vec2 base, vec2 f, vec2 o, vec2 halfTexel, float pixelDepth, inout float sum, inout float wsum) {
    vec2 c = base + o;
    vec4 s = texture(sampler2D(t_half, NVR_SAMPLER(pc.pointSampler)), (c + 0.5) * halfTexel);
    float gap = abs(s.g * cb.cameraData.y - pixelDepth) / (1.0 + 0.01 * pixelDepth);
    float bw = mix(1.0 - f.x, f.x, o.x) * mix(1.0 - f.y, f.y, o.y);
    float w = bw / (1.0 + 16.0 * gap * gap);
    sum += w * s.r;
    wsum += w;
}

void main() {
    vec2 halfTexel = 2.0 * cb.reciprocalResolution.xy;
    float pixelDepth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r * cb.cameraData.y;
    vec2 h = uv / halfTexel - 0.5;
    vec2 base = floor(h);
    vec2 f = h - base;
    float sum = 0.0, wsum = 0.0;
    upTap(base, f, vec2(0.0, 0.0), halfTexel, pixelDepth, sum, wsum);
    upTap(base, f, vec2(1.0, 0.0), halfTexel, pixelDepth, sum, wsum);
    upTap(base, f, vec2(0.0, 1.0), halfTexel, pixelDepth, sum, wsum);
    upTap(base, f, vec2(1.0, 1.0), halfTexel, pixelDepth, sum, wsum);
    float v = clamp(sum / max(wsum, 1e-12), 0.0, 1.0);
    o_color = vec4(v, v, v, 1.0);
}
