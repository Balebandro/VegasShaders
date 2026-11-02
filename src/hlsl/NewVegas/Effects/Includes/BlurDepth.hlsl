#include "Includes/Blur.hlsl"

/*
* A depth aware blur that requires the Depthbuffer and related functions from Depth.hlsl
*/

#ifndef BLUR_FULL_WRITE_ONLY
// perform depth aware 12 taps blur along the direction of the offsetmask
float4 DepthBlur(VSOUT px, uniform sampler2D buf, uniform float2 offset_mask, uniform float blur_radius,uniform float depth_drop,uniform float end_fade) : COLOR0
{
	float weight_sum = 0.114725602f;
	float4 color1_v = tex2D(buf, px.UVCoord) * weight_sum;

	float depth1_v = readDepth(px.UVCoord);
	clip(end_fade - depth1_v);

	depth_drop *= (depth1_v / farZ);

	[unroll]
    for (int i = 0; i < cKernelSize; i++)
    {
		float2 uv = clampToRectIfOn(px.UVCoord + (BlurOffsets[i] * offset_mask) * blur_radius, TESR_ReciprocalResolution.xy);	// UNOFFICIAL 2026-10-27 RenderScale v2
		float4 color2_v = tex2D(buf,  uv);
		float depth2_v = readDepth(uv);
		float diff_v = abs(float(depth1_v - depth2_v));

		int use_for_blur = (diff_v <= depth_drop);
		color1_v += BlurWeights[i] * color2_v * use_for_blur;
		weight_sum += BlurWeights[i] * use_for_blur;
    }
	color1_v /= weight_sum;
    return float4(color1_v.rgb, 1);
}
#endif // BLUR_FULL_WRITE_ONLY

// DepthBlur's result for every pixel, without clip(): beyond endFade it returns its input, which is what clip() left
// there when the frame chain had pre-filled the destination with that input. With no clip() in the effect the chain
// skips those whole-frame copies (one per pass). Same maths in the same order otherwise.
float4 DepthBlurFullValue(float2 uv, uniform sampler2D buf, uniform float2 offset_mask, uniform float blur_radius, uniform float depth_drop, uniform float end_fade)
{
	float4 center_v = tex2Dlod(buf, float4(uv, 0, 0));
	float depth1_v = readDepthLod(uv);
	[branch] if (depth1_v > end_fade) return center_v;

	float weight_sum = 0.114725602f;
	float4 color1_v = center_v * weight_sum;
	depth_drop *= (depth1_v / farZ);

	[unroll]
    for (int i = 0; i < cKernelSize; i++)
    {
		float2 tap_uv = clampToRectIfOn(uv + (BlurOffsets[i] * offset_mask) * blur_radius, TESR_ReciprocalResolution.xy);	// UNOFFICIAL 2026-10-27 RenderScale v2
		float4 color2_v = tex2Dlod(buf, float4(tap_uv, 0, 0));
		float depth2_v = readDepthLod(tap_uv);
		float diff_v = abs(float(depth1_v - depth2_v));

		int use_for_blur = (diff_v <= depth_drop);
		color1_v += BlurWeights[i] * color2_v * use_for_blur;
		weight_sum += BlurWeights[i] * use_for_blur;
    }
	color1_v /= weight_sum;
    return float4(color1_v.rgb, 1);
}

float4 DepthBlurFull(VSOUT px, uniform sampler2D buf, uniform float2 offset_mask, uniform float blur_radius, uniform float depth_drop, uniform float end_fade) : COLOR0
{
	return DepthBlurFullValue(px.UVCoord, buf, offset_mask, blur_radius, depth_drop, end_fade);
}

// Same blur for a pass whose destination is NOT the texture it samples (ping-pong). Pixels
// beyond endFade are passed through instead of clip()ped: clip leaves the destination holding
// whatever it had before, which equals the input only when rendering in place -- otherwise it is
// stale or uninitialised data that the filtered taps of neighbouring pixels bleed into thin
// geometry against the sky.
float4 DepthBlurKeep(VSOUT px, uniform sampler2D buf, uniform float2 offset_mask, uniform float blur_radius,uniform float depth_drop,uniform float end_fade) : COLOR0
{
	float4 center_v = tex2Dlod(buf, float4(px.UVCoord, 0, 0));
	float depth1_v = readDepthLod(px.UVCoord);
	[branch] if (depth1_v > end_fade) return center_v;

	// Fetch the neighbours first. When every tap equals the centre -- fully lit or evenly
	// shadowed, which is most of the screen -- the depth-weighted average is the centre value
	// whatever the weights are, so the twelve depth reads can be skipped with identical output.
	float4 taps_v[cKernelSize];
	float4 spread_v = 0;
	[unroll]
	for (int t = 0; t < cKernelSize; t++) {
		taps_v[t] = tex2Dlod(buf, float4(clampToRectIfOn(px.UVCoord + (BlurOffsets[t] * offset_mask) * blur_radius, TESR_ReciprocalResolution.xy), 0, 0));	// UNOFFICIAL 2026-10-27 RenderScale v2
		spread_v = max(spread_v, abs(taps_v[t] - center_v));
	}
	[branch] if (max(spread_v.r, spread_v.g) < 0.0001f) return float4(center_v.rgb, 1);

	float weight_sum = 0.114725602f;
	float4 color1_v = center_v * weight_sum;
	depth_drop *= (depth1_v / farZ);

	[unroll]
    for (int i = 0; i < cKernelSize; i++)
    {
		float depth2_v = readDepthLod(clampToRectIfOn(px.UVCoord + (BlurOffsets[i] * offset_mask) * blur_radius, TESR_ReciprocalResolution.xy));
		float diff_v = abs(float(depth1_v - depth2_v));

		int use_for_blur = (diff_v <= depth_drop);
		color1_v += BlurWeights[i] * taps_v[i] * use_for_blur;
		weight_sum += BlurWeights[i] * use_for_blur;
    }
	color1_v /= weight_sum;
    return float4(color1_v.rgb, 1);
}
