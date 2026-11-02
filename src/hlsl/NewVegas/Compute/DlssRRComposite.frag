#version 450
// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (needs NVR DXVK; effects/TAAFSR.cpp RayReconstruct, docs/next/FORK2-DLSS-RR.md): the last
// step of the RR point. DLSS-D's output (linear HDR) into the frame chain, encoded back into the stored curve the chain holds before the
// tonemap (pow(o, 1 / Linearization), the inverse of DlssRRGuides' decode), except on the first-person weapon: it is drawn without jitter
// (its own camera), so it is put back exactly as it was before RR, as FSRComposite does after DLSS SR.
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rr;          // RR's output (RGBA16F, linear)
layout(set = 1, binding = 1) uniform texture2D t_source;      // the frame chain's image before RR
layout(set = 1, binding = 2) uniform texture2D t_viewmodel;   // TESR_DepthBufferViewModel (> 0 on the weapon)

layout(push_constant) uniform Push {
    uint pointIndex;            // 0: POINT, CLAMP (the extension writes the heap index)
    float invLinearization;     // 4: 1 / TESR_ToneMapping.w
    uint rawFrame;              // 8: (review 2026-10-30) this frame's RT effects skipped their own denoisers (WantsRaw): the weapon
                                //    takes RR's output too (slightly soft, it is not jittered, but not the raw 1-ray noise on it)
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define POINT_S NVR_SAMPLER(pc.pointIndex)

void main() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
    vec4 source = texelFetch(sampler2D(t_source, POINT_S), pixel, 0);
    bool weapon = pc.rawFrame == 0u && texelFetch(sampler2D(t_viewmodel, POINT_S), pixel, 0).x > 0.0;
    vec3 color = weapon ? source.rgb : pow(max(texelFetch(sampler2D(t_rr, POINT_S), pixel, 0).rgb, vec3(0.0)), vec3(pc.invLinearization));
    o_color = vec4(color, source.a);
}
