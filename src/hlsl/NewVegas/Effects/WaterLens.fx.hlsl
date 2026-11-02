// WaterLens effect (TESReloaded), D3D9 side. One unnamed technique, one pass (index 0).
// The frame is refracted by a drifting, tileable ripple (the water_NRM_lens map) and gets thin bluish glints where the
// ripple bends light most. Strength is Amount times the fade, set by the C++ (WaterLens.cpp); the pass is skipped at zero.
// Vulkan twin: Compute/WaterLens.frag (same maths, same push layout).

float4 TESR_GameTime;		// z: real time in seconds (drives the ripple motion)
float4 TESR_WaterLensData;	// x: TimeMultA, y: TimeMultB, z: Viscosity, w: effective strength (Amount x fade)

// Samplers: slots s0 and s1 as the C++ binds them.
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_waterlensSampler : register(s1) < string ResourceName = "Effects\water_NRM_lens.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

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

float4 WaterLens(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float realTime = TESR_GameTime.z;
	float timeA = realTime * TESR_WaterLensData.x;
	float timeB = realTime * TESR_WaterLensData.y;

	// two noise samples: the first at half the frame coordinate with a slow sideways wobble and drift,
	// the second straight down the frame by real time x TimeMultB
	float2 uvA = uv * 0.5f + float2(0.05f * sin(timeA), 0.05f * timeA);
	float2 uvB = uv + float2(0.0f, timeB);
	float4 noiseA = tex2D(TESR_waterlensSampler, uvA) * 2.0f - 1.0f;
	float4 noiseB = tex2D(TESR_waterlensSampler, uvB) * 2.0f - 1.0f;
	noiseA.b += noiseB.a;

	// refraction direction: Viscosity scales the screen-plane tilt, then normalise (a zero vector stays zero)
	float3 dir = float3(noiseA.x * TESR_WaterLensData.z, noiseA.y * TESR_WaterLensData.z, noiseA.z);
	dir /= max(length(dir), 1.0e-6f);

	float strength = TESR_WaterLensData.w;
	float3 scene = tex2D(TESR_RenderedBuffer, uv + dir.xy * (0.2f * strength)).rgb;

	// glints: the bend in each screen axis, raised to the sixth power as an even power (no NaN for a negative base),
	// clamped to 0..1, summed, and tinted bluish. Added with no clamp: HDR chains may go above 1.
	float2 bend = dir.xy * strength;
	float2 bend2 = bend * bend;
	float2 bend6 = bend2 * bend2 * bend2;
	float glint = saturate(bend6.x) + saturate(bend6.y);

	return float4(scene + glint * float3(0.85f, 0.85f, 1.0f), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 WaterLens();
	}
}
