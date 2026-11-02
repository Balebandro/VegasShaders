// PointShadows technique 2 (LampContact) as a Vulkan pass (needs NVR DXVK): the D3D9 ContactPS of Effects/PointShadows.fx.hlsl,
// line for line. Binding 0 = depth, binding 1 = normals; multiplied onto the mask (blend: destination x source).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;
layout(set = 1, binding = 1) uniform texture2D t_normals;

layout(push_constant) uniform Push {
    uint linearSampler;   // 0
    uint pointSampler;    // 4
    uint mode;            // 8
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float depthAt(vec2 p) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), p, 0.0).r * cb.cameraData.y;
}

void main() {
    float dist = depthAt(uv);
    float fade = clamp((dist - cb.lampContactFade.x) * cb.lampContactFade.y, 0.0, 1.0) *
        clamp((cb.lampContactFade.z - dist) * cb.lampContactFade.w, 0.0, 1.0);
    if (fade <= 0.0) { o_color = vec4(1.0); return; }

    // Lamp selection (world space): the brightest of slots 0..10 by (1 - s)^2 x light, over the light of all slots 0..11.
    vec3 pos = cb.cameraPosition.xyz + toWorldDir(uv) * dist;
    float total = 0.0, best = 0.0, bestRadius = 0.0;
    vec3 bestLamp = vec3(0.0);
    for (int k = 0; k < 12; k++) {
        vec4 lp = cb.shadowLightPosition[k];
        vec4 col = cb.lightColor[k];
        if (!(lp.w > 0.0)) continue;
        vec3 t = lp.xyz - pos;
        float s2 = clamp(dot(t, t) / (lp.w * lp.w), 0.0, 1.0);
        float share = (1.0 - s2) * (1.0 - s2) * lumaOf(col.rgb) * col.w;
        total += share;
        if (k < 11 && share > best) { best = share; bestRadius = lp.w; bestLamp = lp.xyz; }
    }
    if (bestRadius <= 0.0 || best <= cb.lampContactBias.w * total) { o_color = vec4(1.0); return; }

    // View space from here on: D3D mul(float4(v,0), M) is M * v here.
    vec3 viewPos = (cb.viewTransform * vec4(toWorldDir(uv) * dist, 0.0)).xyz;
    vec3 nView = texture(sampler2D(t_normals, NVR_SAMPLER(pc.pointSampler)), uv).xyz * 2.0 - 1.0;
    vec3 toLampWorld = bestLamp - pos;
    float lampDist = max(length(toLampWorld), 1e-6);
    vec3 dirView = (cb.viewTransform * vec4(toLampWorld / lampDist, 0.0)).xyz;
    dirView = dirView * inversesqrt(max(dot(dirView, dirView), 1e-8));
    if (dot(dirView, nView) < 0.1) { o_color = vec4(1.0); return; }

    float marchLength = min(cb.lampContact.y, 0.5 * lampDist);
    vec3 start = viewPos + nView * (cb.lampContact.w + cb.lampContactBias.z * dist);
    float ign = fract(cb.lampContactNoise.z * fract(dot(uv, cb.lampContactNoise.xy)));
    float jitter = cb.lampContactNoise.w > 0.5 ? ign : 0.5;

    float blocked = 0.0;
    for (int i = 0; i < 6; i++) {
        float t = (float(i) + jitter) / 6.0;
        vec3 p = start + dirView * (marchLength * t);
        if (p.z <= 1e-3) break;
        vec4 q = cb.projectionTransform * vec4(p, 1.0);	// D3D mul(float4(p,1), P)
        vec3 proj = q.xyz / q.w;
        vec2 sp = vec2(proj.x * 0.5 + 0.5, 0.5 - 0.5 * proj.y);
        if (any(lessThan(sp, vec2(0.0))) || any(greaterThan(sp, vec2(1.0)))) break;
        float stored = depthAt(sp);
        float gap = p.z - stored;
        float bias = cb.lampContactBias.x + cb.lampContactBias.y * p.z;
        if (gap > bias && gap < cb.lampContact.z) blocked = max(blocked, 1.0 - t);
    }

    float shade = cb.lampContact.x * fade * blocked * (best / max(total, 1e-6));
    float v = clamp(1.0 - shade, 0.0, 1.0);
    o_color = vec4(v, v, v, 1.0);
}
