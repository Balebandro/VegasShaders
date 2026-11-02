#define cKernelSize (12)
static const float2 OffsetMaskH = float2(1.0, 0.0);
static const float2 OffsetMaskV = float2(0.0, 1.0);

static const float BlurWeights[ cKernelSize ] = 
{
	0.057424882f, 0.058107773f, 0.061460144f, 0.071020611f,
	0.088092873f, 0.106530916f, 0.106530916f, 0.088092873f,
	0.071020611f, 0.061460144f, 0.058107773f, 0.057424882f
};
 
static const float2 BlurOffsets[ cKernelSize ] = 
{
	float2(-6.0f*TESR_ReciprocalResolution.x, -6.0f*TESR_ReciprocalResolution.y),
	float2(-5.0f*TESR_ReciprocalResolution.x, -5.0f*TESR_ReciprocalResolution.y),
	float2(-4.0f*TESR_ReciprocalResolution.x, -4.0f*TESR_ReciprocalResolution.y),
	float2(-3.0f*TESR_ReciprocalResolution.x, -3.0f*TESR_ReciprocalResolution.y),
	float2(-2.0f*TESR_ReciprocalResolution.x, -2.0f*TESR_ReciprocalResolution.y),
	float2(-1.0f*TESR_ReciprocalResolution.x, -1.0f*TESR_ReciprocalResolution.y),
	float2(1.0f*TESR_ReciprocalResolution.x, 1.0f*TESR_ReciprocalResolution.y),
	float2(2.0f*TESR_ReciprocalResolution.x, 2.0f*TESR_ReciprocalResolution.y),
	float2(3.0f*TESR_ReciprocalResolution.x, 3.0f*TESR_ReciprocalResolution.y),
	float2(4.0f*TESR_ReciprocalResolution.x, 4.0f*TESR_ReciprocalResolution.y),
	float2(5.0f*TESR_ReciprocalResolution.x, 5.0f*TESR_ReciprocalResolution.y),
	float2(6.0f*TESR_ReciprocalResolution.x, 6.0f*TESR_ReciprocalResolution.y)
};

// The helpers below clip() pixels. An effect that does not use them can define BLUR_FULL_WRITE_ONLY before the include
// to leave them out: EffectRecord scans the preprocessed source for clip/discard and, when it finds any, makes the frame
// chain copy the whole frame into every pass's destination first.
#include "Includes/RenderRect.hlsl"	// UNOFFICIAL 2026-10-27 RenderScale v2: the scaled part is scaleFactor of the drawn part (TESR_RenderScale.xy). The game compiles effects with Effects\ as the include root: "Includes/" is needed (fxc alone also finds the file next to this one)
#ifndef BLUR_FULL_WRITE_ONLY
// downsample/upsample a part of the screen given by the scaleFactor
float4 Scale(VSOUT px, uniform sampler2D buf, uniform float scale_factor) : COLOR0
{
	float2 uv = px.UVCoord / scale_factor;// scale the uv by wanted scale
	clip((uv < TESR_RenderScale.xy) - 1); // discard uvs outside of [0-1]range (of the drawn part)

	return tex2D(buf, uv);
}

// simple local average without weights. Use scaleFactor to only blur a portion of the screen starting from the top left corner
float4 BoxBlur (VSOUT px, uniform sampler2D buf, uniform float2 offset_mask, uniform float scale_factor) :COLOR0
{
	float2 part_v = scale_factor * TESR_RenderScale.xy;
	clip((px.UVCoord <= part_v) - 1);

	float2 maxuv_v = part_v - 1.5 * TESR_ReciprocalResolution.xy;
	float4 color = tex2D(buf, px.UVCoord);
	color += tex2D(buf, min(px.UVCoord + offset_mask * 1 * TESR_ReciprocalResolution.xy, maxuv_v));
	color += tex2D(buf, min(px.UVCoord + offset_mask * -1 * TESR_ReciprocalResolution.xy, maxuv_v));
	color += tex2D(buf, min(px.UVCoord + offset_mask * -2 * TESR_ReciprocalResolution.xy, maxuv_v));
	color += tex2D(buf, min(px.UVCoord + offset_mask * 2 * TESR_ReciprocalResolution.xy, maxuv_v));

	// return color;
	return float4(color.rgb /= 5, 1);
}

float4 Blur(VSOUT px, uniform sampler2D buf,  uniform float2 offset_mask, uniform float blur_radius, uniform float scl) : COLOR0
{
	// blur using a gaussian blur
	float weight_sum = 0.114725602f;
	float2 uv = px.UVCoord;
	float4 color = tex2D(buf, uv) * weight_sum;
	float2 part_v = scl * TESR_RenderScale.xy;	// UNOFFICIAL 2026-10-27 RenderScale v2
	clip ((uv <= part_v) - 1);

    for (int tap = 0; tap < cKernelSize; tap++)
    {
		float2 uv_off = (BlurOffsets[tap] * offset_mask) * blur_radius;
		float is_valid = ((uv.x + uv_off.x) < part_v.x) && ((uv.y + uv_off.y) < part_v.y); //discard samples outside the screen area
		color += BlurWeights[tap] * tex2D(buf, uv + uv_off) * is_valid;
		weight_sum += BlurWeights[tap] * is_valid;
    }
	color /= weight_sum;
    return float4(color.rgb, 1.0);
}
#endif // BLUR_FULL_WRITE_ONLY
