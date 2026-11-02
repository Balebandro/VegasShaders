// Interior shadows, blur pass as a Vulkan pass (needs NVR DXVK). The D3D9 DedicatedInteriorShadows blur of
// Effects/ShadowsInteriors.fx.hlsl (BlurTerm), line for line; run twice: horizontal (offsetMask 1,0) then vertical (0,1).
// Not clamped to a render-scale rect: the C++ keeps this path off while a rect is active.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_mask;    // the mask (horizontal) or scratch A (vertical)
layout(set = 1, binding = 1) uniform texture2D t_depth;   // TESR_DepthBuffer (stored: depth over the far distance)

layout(push_constant) uniform Push {
    vec4 cameraData;            // 0: y = far distance
    vec4 reciprocalResolution;  // 16: texel size
    vec2 offsetMask;            // 32: (1, 0) horizontal, (0, 1) vertical
    float maxDistance;          // 40: draw distance (TESR_ShadowFade.w)
    uint samplerIndex;          // 44: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// The same twelve taps (offsets -6..-1, +1..+6) and centre weight as the .fx.hlsl.
const float kWeight[12] = float[12](0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873, 0.106530916,
    0.106530916, 0.088092873, 0.071020611, 0.061460144, 0.058107773, 0.057424882);
const float kCentreWeight = 0.114725602;

float depthAt(vec2 p) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), p, 0.0).r * pc.cameraData.y;
}

float maskAt(vec2 p) {
    return texture(sampler2D(t_mask, NVR_SAMPLER(pc.samplerIndex)), p).r;
}

void main() {
    float farZ = pc.cameraData.y;
    float centreDepth = depthAt(uv);
    if (centreDepth >= pc.maxDistance) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float threshold = 3500.0 * (centreDepth / farZ);
    float sum = maskAt(uv) * kCentreWeight;
    float wsum = kCentreWeight;
    for (int i = 0; i < 12; i++) {
        float off = i < 6 ? float(i) - 6.0 : float(i) - 5.0;
        vec2 tuv = uv + pc.offsetMask * off * pc.reciprocalResolution.xy;
        if (abs(depthAt(tuv) - centreDepth) < threshold) {
            sum += kWeight[i] * maskAt(tuv);
            wsum += kWeight[i];
        }
    }
    o_color = vec4(sum / wsum, 0.0, 0.0, 1.0);
}
