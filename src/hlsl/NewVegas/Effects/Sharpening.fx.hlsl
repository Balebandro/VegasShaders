// Sharpening: local edge contrast. Each pixel is pushed away from the average of its 3 x 3 neighbourhood, by an amount that
// follows the local contrast (dark pixels count less), fades out with the depth (full at the camera, none at Offset x 1000 game
// units and beyond, so the sky and far land are left alone), and is blended by that fade.

float4 TESR_SharpeningData;		// x strength (the boost is 2 x strength), y clamp (not used), z offset (fade distance = z x 1000), w unused
float4 TESR_ReciprocalResolution;	// xy: 1 / size
float4 TESR_CameraData;			// y: far plane distance (game units)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer    : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 SharpenPass(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float4 centre = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));

	// Fade with depth: 1 at the camera, 0 at Offset x 1000 and beyond (0 when Offset is 0 or less: nothing is sharpened).
	float maxDepth = TESR_SharpeningData.z * 1000.0f;
	float depth = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x * TESR_CameraData.y;
	float fade = saturate((maxDepth - depth) / max(maxDepth, 0.001f));
	fade = (maxDepth > 0.0f) ? pow(fade, 4.0f) : 0.0f;

	// 3 x 3 neighbourhood, weighted by Manhattan distance: centre 1, edge neighbours 2/3, corners 1/3.
	float3 sum = 0.0f;
	float weightSum = 0.0f;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			float w = (3.0f - abs((float)x) - abs((float)y)) / 3.0f;
			float2 t = uv + float2((float)x, (float)y) * TESR_ReciprocalResolution.xy;
			sum += tex2Dlod(TESR_RenderedBuffer, float4(t, 0.0f, 0.0f)).rgb * w;
			weightSum += w;
		}
	}
	float3 average = sum / weightSum;
	float3 diff = centre.rgb - average;

	// Local contrast, weighted by luma so dark noise is not sharpened.
	float L = dot(centre.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	float lumaWeight = L / (L + 0.05f);
	float contrast = (abs(diff.r) + abs(diff.g) + abs(diff.b)) * lumaWeight;
	float boost = saturate(contrast * 2.0f * TESR_SharpeningData.x);

	float3 sharpened = centre.rgb + diff * boost;
	// The 8-bit target clips the result on write anyway; clamping here keeps the chain free of out-of-range values.
	return float4(saturate(lerp(centre.rgb, sharpened, fade)), centre.a);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 SharpenPass();
	}
}
