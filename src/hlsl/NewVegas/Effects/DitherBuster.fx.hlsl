// DitherBuster: removes the dither that alpha-tested foliage leaves under DXVK. An isolated pixel (one whose luma differs from its four
// neighbours in a checkerboard way, not along an edge) is blended toward the mean of its five-tap neighbourhood.
// DitherOnly (technique 0) is the plain pass. DitherOnlyStable (technique 1) adds a soft gate, a de-dither judgement and a debug view.

float4 TESR_ReciprocalResolution;  // xy: 1 / size of the picture
float4 TESR_DitherBusterData;      // x: Strength; y: MaskPower (not used by either technique); z: DitherThreshold; w: Radius (not used)
float4 TESR_DitherBusterGate;      // technique 1: x GateSoftness (0..1); y DeDither (> 0.5 on); z DebugView (> 0.5 on)
float4 TESR_ToneMappingClip;       // technique 1: y the tonemap's dither size (colour units); z its frame counter. Set by the tonemap.

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// The tonemap's noise, recomputed per pixel (LumaWithoutDither, TonemapDitherAt). Needs TESR_ToneMappingClip above.
#include "Includes/DitherNoise.hlsl"
// RenderScale: the neighbours are kept inside the drawn part of a rect frame (no-op on native frames).
#include "Includes/RenderRect.hlsl"

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float Luma709(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// The five taps of the pass: the centre and its four direct neighbours, one texel away (kept inside the drawn part).
struct Neighbourhood
{
	float3 centre, up, down, left, right;
};

Neighbourhood ReadNeighbourhood(float2 uv)
{
	const float2 t = TESR_ReciprocalResolution.xy;
	Neighbourhood n;
	n.centre = tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv, t)).rgb;
	n.up     = tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv + float2(0.0f, -t.y), t)).rgb;
	n.down   = tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv + float2(0.0f,  t.y), t)).rgb;
	n.left   = tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv + float2(-t.x, 0.0f), t)).rgb;
	n.right  = tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv + float2( t.x, 0.0f), t)).rgb;
	return n;
}

// How isolated the centre is: the neighbours' differences from it, minus the central-difference gradient (four times its length).
// On a step edge the two cancel (about 0); on a one-pixel checkerboard the gradient is 0 and the differences stay large.
float Isolation(float lc, float lu, float ld, float ll, float lr)
{
	const float differences = abs(lc - lu) + abs(lc - ld) + abs(lc - ll) + abs(lc - lr);
	const float2 gradient = float2(lr - ll, lu - ld) * 0.5f;
	return max(0.0f, differences - 4.0f * length(gradient));
}

// Technique 0: the plain pass. Hard threshold; the isolated centre is blended toward the five-tap mean by Strength.
float4 DitherOnlyPS(VSOUT IN) : COLOR0
{
	const Neighbourhood n = ReadNeighbourhood(IN.UVCoord);
	const float threshold = TESR_DitherBusterData.z;
	const float strength = saturate(TESR_DitherBusterData.x);
	const float iso = Isolation(Luma709(n.centre), Luma709(n.up), Luma709(n.down), Luma709(n.left), Luma709(n.right));

	const float3 mean = (n.centre + n.up + n.down + n.left + n.right) * 0.2f;
	const bool isolated = threshold > 0.0f && iso > threshold;
	return float4(isolated ? lerp(n.centre, mean, strength) : n.centre, 1.0f);
}

// Technique 1: the same pass with the soft gate, the de-dither judgement and the debug view.
float4 DitherOnlyStablePS(VSOUT IN, float2 vpos : VPOS) : COLOR0
{
	const Neighbourhood n = ReadNeighbourhood(IN.UVCoord);
	const float2 pixel = floor(vpos);   // whole pixels, the tonemap's convention (DitherNoise.hlsl)
	const float softness = TESR_DitherBusterGate.x;
	const bool deDither = TESR_DitherBusterGate.y > 0.5f;
	const bool debugView = TESR_DitherBusterGate.z > 0.5f;

	const float lc = Luma709(n.centre);
	if (debugView) {
		// The picture with the recomputed tonemap noise taken out, in the same proportion the tonemap added it.
		const float3 clean = n.centre - TonemapDitherAt(pixel) * saturate(lc * 128.0f);
		return float4(saturate(clean), 1.0f);
	}

	// De-dither: the decision (only) is judged on the luma with the noise taken out at each tap's own pixel.
	float lu = Luma709(n.up), ld = Luma709(n.down), ll = Luma709(n.left), lr = Luma709(n.right);
	float lcJudge = lc;
	if (deDither) {
		lcJudge = LumaWithoutDither(lc, pixel);
		lu = LumaWithoutDither(lu, pixel + float2(0.0f, -1.0f));
		ld = LumaWithoutDither(ld, pixel + float2(0.0f,  1.0f));
		ll = LumaWithoutDither(ll, pixel + float2(-1.0f, 0.0f));
		lr = LumaWithoutDither(lr, pixel + float2( 1.0f, 0.0f));
	}
	const float iso = Isolation(lcJudge, lu, ld, ll, lr);

	// Gate: a hard step at the threshold, or a smooth ramp across it when GateSoftness is above 0.
	const float threshold = TESR_DitherBusterData.z;
	float gate = 0.0f;
	if (threshold > 0.0f) {
		gate = (softness > 0.0f) ? smoothstep(threshold * (1.0f - softness), threshold * (1.0f + softness), iso)
		                         : (iso > threshold ? 1.0f : 0.0f);
	}
	const float3 mean = (n.centre + n.up + n.down + n.left + n.right) * 0.2f;
	const float weight = saturate(TESR_DitherBusterData.x) * gate;
	return float4(lerp(n.centre, mean, weight), 1.0f);
}

technique DitherOnly
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DitherOnlyPS();
	}
}

technique DitherOnlyStable
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DitherOnlyStablePS();
	}
}
