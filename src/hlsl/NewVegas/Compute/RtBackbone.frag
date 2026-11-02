// UNOFFICIAL 2026-10-31 the RT denoisers' temporal backbone ([Main.Main.RayTracing] DenoiseBackbone, core\RtDenoise.cpp;
// docs\next\RT-DENOISE-BACKBONE.md; docs\next\ROOT-ASSUMPTIONS-PLAN.md D): made ONCE a frame, full size, before the first RT denoiser
// runs, and read by all of them (sun, lamps, AO, GI). Per pixel: where this surface was on screen last frame (the camera's move AND the
// object's own move, B1's TESR_ObjectMotionBuffer, as ReBLUR / NRD take motion vectors), and whether last frame saw this surface there
// at all (disocclusion: last frame's depth at that place does not match this point's depth as last frame's camera saw it; off screen;
// a camera cut). Output RGBA16F: rg = last frame's uv - this uv, b = 1 disoccluded, a = view z / farZ (last frame's depth, next frame).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtBackbone.spv RtBackbone.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (.r = view z / farZ)
layout(set = 1, binding = 1) uniform texture2D t_previous;   // last frame's backbone (.a = its view z / farZ), or a stand-in (move.w 1)
layout(set = 1, binding = 2) uniform texture2D t_motion;     // TESR_ObjectMotionBuffer (rgb = the surface's world move since last frame), or a stand-in

layout(push_constant) uniform Push {
    mat4 prevViewProj;   // last frame's camera-relative view-projection (as RtDenoiseTemporal.frag)
    vec4 forward;        // xyz camera forward, w farZ
    vec4 right;          // xyz right / P00
    vec4 up;             // xyz up / P11
    vec4 move;           // xyz this frame's camera position - last frame's (world units), w 1: no last frame (all disoccluded)
    vec4 params;         // x depth tolerance (relative), y 1: object motion (t_motion) is this frame's
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    const float d01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    if (!(d01 > 0.0 && d01 < 0.9999)) { o_color = vec4(0.0, 0.0, 0.0, d01); return; }   // sky: never a history problem here
    const float viewZ = d01 * pc.forward.w;
    vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * uv.x - 1.0) - pc.up.xyz * (2.0 * uv.y - 1.0)) * viewZ + pc.move.xyz;
    if (pc.params.y > 0.5) p -= textureLod(sampler2D(t_motion, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
    const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
    if (pc.move.w > 0.5 || clip.w <= 0.0) { o_color = vec4(0.0, 0.0, 1.0, d01); return; }
    const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    float disoccluded = 1.0;
    if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
        const float prevZ = textureLod(sampler2D(t_previous, NVR_SAMPLER(pc.samplerIndex)), prevUv, 0.0).a * pc.forward.w;
        disoccluded = prevZ > 0.0 && abs(prevZ - clip.w) <= pc.params.x * max(clip.w, 1.0) ? 0.0 : 1.0;
    }
    o_color = vec4(prevUv - uv, disoccluded, d01);
}
