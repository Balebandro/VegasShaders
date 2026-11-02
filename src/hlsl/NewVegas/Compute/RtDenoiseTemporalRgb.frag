// UNOFFICIAL 2026-10-29 shared RT denoiser (core\RtDenoise.h), RGB mode (Settings.Channels 3: GI / sky colour), pass 1: temporal
// accumulation as RtDenoiseTemporal.frag, on rgb. Two outputs: 0 = (rgb, history length), 1 = (luma second moment, view depth, 0, 0).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseTemporalRgb.spv RtDenoiseTemporalRgb.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_noisy;      // rgb
layout(set = 1, binding = 1) uniform texture2D t_historyA;   // last frame's output 0
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_historyB;   // last frame's output 1
layout(set = 1, binding = 4) uniform texture2D t_motion;     // UNOFFICIAL 2026-10-31 DenoiseBackbone: as RtDenoiseTemporal.frag
layout(set = 1, binding = 5) uniform texture2D t_backbone;

layout(push_constant) uniform Push {
    mat4 prevViewProj;
    vec4 forward;        // w farZ
    vec4 right;
    vec4 up;
    vec4 move;           // xyz camera move, w 1: reset
    vec4 params;         // x max history, y depth tolerance
    vec4 backbone;       // x 1: subtract the object motion, y 1: the shared disocclusion mask restarts, z the history cap on moving
                         // pixels (0 none), w 1: half size (the mask's 2 x 2)
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;

void main() {
    const vec3 value = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
    const float l = luma(value);
    const float viewZ = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r * pc.forward.w;
    vec4 a = vec4(value, 1.0);
    vec4 b = vec4(l * l, viewZ, 0.0, 0.0);
    // (2026-10-31, rt-ao-sky) at half size (backbone.w 1) the mask over this pixel's 2 x 2 full-size pixels (uv sits on their corner):
    // a thin newly seen edge is not missed by the one tap a point sample takes
    const bool fresh = pc.backbone.y > 0.5 && (pc.backbone.w > 0.5
        ? any(greaterThan(textureGather(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 2), vec4(0.5)))
        : textureLod(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).b > 0.5);
    if (pc.move.w < 0.5 && viewZ > 0.0 && !fresh) {
        vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * uv.x - 1.0) - pc.up.xyz * (2.0 * uv.y - 1.0)) * viewZ + pc.move.xyz;
        float cap = pc.params.x;
        if (pc.backbone.x > 0.5) {
            const vec3 own = textureLod(sampler2D(t_motion, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
            p -= own;
            // (2026-10-31, rt-ao-sky) RT GI: a surface that moves itself (a walking person) has its bounce light change as it moves:
            // a short history there (backbone.z frames, GIMovingHistory) instead of 16 frames trailing behind it
            if (pc.backbone.z > 0.5 && dot(own, own) > 0.0625) cap = min(cap, pc.backbone.z);
        }
        const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
        if (clip.w > 0.0) {
            const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                const vec4 ha = textureLod(sampler2D(t_historyA, NVR_SAMPLER(pc.samplerIndex)), prevUv, 0.0);
                const vec4 hb = textureLod(sampler2D(t_historyB, NVR_SAMPLER(pc.samplerIndex)), prevUv, 0.0);
                if (abs(hb.g - clip.w) <= pc.params.y * max(clip.w, 1.0) && ha.a > 0.0) {
                    const float length = min(min(ha.a, cap) + 1.0, pc.params.x);
                    const float alpha = 1.0 / length;
                    a = vec4(mix(ha.rgb, value, alpha), length);
                    b.r = mix(hb.r, l * l, alpha);
                }
            }
        }
    }
    o_a = a;
    o_b = b;
}
