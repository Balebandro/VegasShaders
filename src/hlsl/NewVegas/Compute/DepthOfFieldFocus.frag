// DepthOfField autofocus pass 1 as a Vulkan pass (needs NVR DXVK): the focus pass of Effects/DepthOfField.fx.hlsl, line for line.
// Writes the near (red) and far (green) blur amounts. Reads the depth at the JitterFix offset and AvgLuma's 1 x 1 blue value.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (linear depth / farZ)
layout(set = 1, binding = 1) uniform texture2D t_avgLuma;    // TESR_AvgLumaBuffer (1 x 1)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 duv = uv - pc.jitter;
    float rawDepth = DOF_SAMPLE(t_depth, duv).x;
    float d = rawDepth * pc.cameraData.y;

    float H = max(pc.data.x * 1000.0, DOF_TINY);
    float f = DOF_SAMPLE(t_avgLuma, vec2(0.5)).b * H;
    float nearLimit = max(f * (H - 0.01) / max(H + f - 0.02, DOF_TINY), DOF_TINY);
    float farLimit = (H - f > DOF_TINY) ? f * (H - 0.01) / (H - f) : 1.0e6;
    farLimit = max(farLimit, DOF_TINY);

    float nearW = clamp(1.0 - d / nearLimit, 0.0, 1.0) * dofNearCutAtten(d);
    float farW = clamp((d - farLimit) / farLimit, 0.0, 1.0) * 0.8;
    farW *= 1.0 - step(DOF_SKY_DEPTH, rawDepth);
    farW = clamp(farW + dofDistantAmount(d, uv), 0.0, 1.0);
    o_color = vec4(nearW, farW, 1.0, 1.0);
}
