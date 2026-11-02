// DepthOfField's distant blur as a Vulkan pass (needs NVR DXVK): Effects/DepthOfField.fx.hlsl technique DistantBlurOnly, line for line.
// effects/DepthOfField.cpp ExtRender (technique 1). Native frames only (no render-rectangle maths).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (linear depth / farZ)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    const vec2 diagonal[4] = vec2[4](vec2(1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(-1.0, -1.0));
    vec2 duv = uv - pc.jitter;
    vec4 centre = DOF_SAMPLE(t_rendered, uv);
    float dc = DOF_SAMPLE(t_depth, duv).x * pc.cameraData.y;
    float blur = dofDistantAmount(dc, uv);
    if (blur <= 0.0) {
        o_color = centre;
        return;
    }
    float radius = pc.blur.w * (1.0 + blur);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; k++) {
        vec2 t = uv + diagonal[k] * radius * pc.reciprocalResolution.xy;
        float td = DOF_SAMPLE(t_depth, t - pc.jitter).x * pc.cameraData.y;
        float w = 1.0 - smoothstep(0.07, 0.22, abs(td - dc) / max(dc, 1.0));
        sum += DOF_SAMPLE(t_rendered, t) * w;
        wsum += w;
    }
    vec4 blurred = centre;
    if (wsum > DOF_TINY) blurred = sum / wsum;
    o_color = mix(centre, blurred, blur);
}
