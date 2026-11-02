// UNOFFICIAL 2026-10-31 RT GI's object motion for ReSTIR GI's temporal reuse ([Main.Main.RayTracing] DenoiseBackbone; core\RtDenoise.cpp
// GIMotion; docs\next\RT-DENOISE-BACKBONE.md "RT GI"). The GI reuse (Rt\RtAOTrace.comp ReuseGI) finds a texel's reservoir of last frame
// through the camera's move only: a walking NPC's samples were looked up where it no longer is. Per trace-grid texel (the target: the
// GI's frame light, NVR_RTGIFrameLight, alpha only; its rgb is the lit frame), how many texels the surface's OWN move (B1's
// TESR_ObjectMotionBuffer) shifts its place on last frame's screen beyond what the camera's move does: dx, dy in -15..15, written as
// (dx + 16) + 32 (dy + 16) (whole numbers up to 1023: exact in a half float). The trace adds it to the reprojected texel.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtGIMotion.spv RtGIMotion.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (.r = view z / farZ)
layout(set = 1, binding = 1) uniform texture2D t_motion;     // TESR_ObjectMotionBuffer (rgb = the surface's world move since last frame)

layout(push_constant) uniform Push {
    mat4 prevViewProj;   // last frame's camera-relative view-projection (as RtDenoiseTemporal.frag)
    vec4 forward;        // xyz camera forward, w farZ
    vec4 right;          // xyz right / P00
    vec4 up;             // xyz up / P11
    vec4 move;           // xyz this frame's camera position - last frame's (world units)
    vec4 params;         // xy the trace grid's size
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec2 PrevUv(vec3 p) {
    const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
    return clip.w > 0.0 ? vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : vec2(-1.0);
}

void main() {
    o_color = vec4(0.0, 0.0, 0.0, 528.0);   // no shift (dx = dy = 0)
    const float d01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    if (!(d01 > 0.0 && d01 < 0.9999)) return;
    const vec3 own = textureLod(sampler2D(t_motion, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
    if (!(dot(own, own) > 1.0e-4) || any(isnan(own))) return;
    const vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * uv.x - 1.0) - pc.up.xyz * (2.0 * uv.y - 1.0)) * (d01 * pc.forward.w) + pc.move.xyz;
    const vec2 camera = PrevUv(p), both = PrevUv(p - own);
    if (camera.x < -0.5 || both.x < -0.5) return;
    const vec2 shift = clamp(round((both - camera) * pc.params.xy), vec2(-15.0), vec2(15.0));
    o_color.a = (shift.x + 16.0) + 32.0 * (shift.y + 16.0);
}
