// Flashlight's two shadow blur passes (DepthBlur(TESR_RenderedBuffer, OffsetMaskH / OffsetMaskV, 1.0, 350,
// TESR_SpotLightPosition.w * 2)) as a Vulkan pass (needs NVR DXVK): Includes/BlurDepth.hlsl and Blur.hlsl line for line.
// effects/Flashlight.cpp ExtRender. DepthBlur's clip() keeps what the target held: the technique runs with ClearRenderTarget, so
// that is the clear colour (0, 0, 0, 1), written here instead.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"

const int cKernelSize = 12;   // Blur.hlsl cKernelSize

// Blur.hlsl BlurWeights
const float blurWeights[cKernelSize] = float[cKernelSize](
    0.057424882,
    0.058107773,
    0.061460144,
    0.071020611,
    0.088092873,
    0.106530916,
    0.106530916,
    0.088092873,
    0.071020611,
    0.061460144,
    0.058107773,
    0.057424882
);

// Blur.hlsl BlurOffsets: -6..-1 then +1..+6 times TESR_ReciprocalResolution (no zero tap)
vec2 blurOffset(int i) {
    float k = i < 6 ? float(i - 6) : float(i - 5);
    return vec2(k * cb.reciprocalResolution.x, k * cb.reciprocalResolution.y);
}

void main() {
    const float blurRadius = 1.0;
    float depthDrop = 350.0;
    float endFade = cb.spotLightPosition.w * 2.0;

    float WeightSum = 0.114725602;
    vec4 color1 = LINEAR_LOD(t_rendered, uv) * WeightSum;

    float depth1 = readDepth(uv);
    if (endFade - depth1 < 0.0) {   // clip(endFade - depth1)
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    depthDrop *= (depth1 / cb.cameraData.y);

    for (int i = 0; i < cKernelSize; i++) {
        vec2 tapUV = uv + (blurOffset(i) * pc.offsetMask) * blurRadius;
        vec4 color2 = LINEAR_LOD(t_rendered, tapUV);
        float depth2 = readDepth(tapUV);
        float diff = abs(depth1 - depth2);

        float useForBlur = (diff <= depthDrop) ? 1.0 : 0.0;
        color1 += blurWeights[i] * color2 * useForBlur;
        WeightSum += blurWeights[i] * useForBlur;
    }
    color1 /= WeightSum;
    o_color = vec4(color1.rgb, 1.0);
}
