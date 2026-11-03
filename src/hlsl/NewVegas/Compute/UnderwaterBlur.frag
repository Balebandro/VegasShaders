// Underwater passes 2 and 3, the god rays' blur (needs NVR DXVK): Includes/Blur.hlsl Blur(TESR_RenderedBuffer, mask, 2, 0.5) line for
// line; pass 2 uses OffsetMaskV (0, 1), pass 3 OffsetMaskH (1, 0). Outside the upper left quarter (uv > 0.5) the pass keeps the
// current image, as the D3D9 pass's pre-fill did. effects/Underwater.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"
#include "Underwater.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec2 offsetMask;      // 0: OffsetMaskV or OffsetMaskH
    uint samplerIndex;    // 8: LINEAR, CLAMP
    uint pad;             // 12
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Blur.hlsl BlurWeights (cKernelSize 12)
const float blurWeights[12] = float[12](
    0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873, 0.106530916,
    0.106530916, 0.088092873, 0.071020611, 0.061460144, 0.058107773, 0.057424882);

void main() {
    const float scale = 0.5;
    // clip((uv <= scale) - 1): the pixels outside the quarter keep the current image
    if (any(greaterThan(uv, vec2(scale)))) {
        o_color = texelFetch(t_rendered, ivec2(gl_FragCoord.xy), 0);
        return;
    }

    const float blurRadius = 2.0;

    // blur using a gaussian blur
    float weightSum = 0.114725602;
    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv) * weightSum;

    for (int i = 0; i < 12; i++) {
        // Blur.hlsl BlurOffsets: k * TESR_ReciprocalResolution for k = -6..-1, 1..6
        float k = (i < 6) ? float(i - 6) : float(i - 5);
        vec2 uvOff = ((k * cb.reciprocalResolution.xy) * pc.offsetMask) * blurRadius;
        float isValid = float(((uv.x + uvOff.x) < scale) && ((uv.y + uvOff.y) < scale)); // discard samples outside the screen area
        color += blurWeights[i] * texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + uvOff) * isValid;
        weightSum += blurWeights[i] * isValid;
    }
    color /= weightSum;

    o_color = vec4(color.rgb, 1.0);
}
