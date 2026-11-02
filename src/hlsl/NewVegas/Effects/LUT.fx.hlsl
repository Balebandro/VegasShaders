// LUT: colour grading through strip look-up tables (N cells of N x N texels side by side, see effects/LUT.cpp).
// Exteriors blend the day and night tables by the time of day; interiors use the interior table. One pass (technique 0).

float4 TESR_LUTData;   // x: day table cell count N (0 = no usable table); y: Strength; z: night N; w: interior N
float4 TESR_LUTBlend;  // x: day/night blend in exteriors (1 = day); y: interior flag; z: HDR compatibility flag (1 = on)

sampler2D TESR_RenderedBuffer    : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTDayBuffer      : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTNightBuffer    : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTInteriorBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

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

// The strip has N cells in a row, each N x N: red runs along the cell, green down it, blue picks the cell (interpolated between the two
// neighbouring cells). Texel centres are used, so the table's first and last entries are read exactly.
float3 LutStrip(sampler2D table, float cells, float3 c)
{
	const float b = c.b * (cells - 1.0f);
	const float cellLo = floor(b);
	const float cellHi = min(cellLo + 1.0f, cells - 1.0f);
	const float blendB = b - cellLo;
	const float2 inCell = float2(c.r * (cells - 1.0f), c.g * (cells - 1.0f));
	const float2 uvLo = float2((cellLo * cells + 0.5f + inCell.x) / (cells * cells), (0.5f + inCell.y) / cells);
	const float2 uvHi = float2((cellHi * cells + 0.5f + inCell.x) / (cells * cells), (0.5f + inCell.y) / cells);
	return lerp(tex2D(table, uvLo).rgb, tex2D(table, uvHi).rgb, blendB);
}

// A table with fewer than two cells is not usable: its colour is the input (the pass passes it through for that table).
float3 GradeWith(sampler2D table, float cells, float3 c)
{
	return (cells >= 2.0f) ? LutStrip(table, cells, saturate(c)) : c;
}

float4 LUT(VSOUT IN) : COLOR0
{
	const float3 original = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// HDR compatibility: a picture above 1 is scaled into the table's range for the lookup and scaled back after it.
	const bool hdr = TESR_LUTBlend.z > 0.5f;
	const float scale = hdr ? max(1.0f, max(original.r, max(original.g, original.b))) : 1.0f;
	const float3 lookup = original / scale;

	const float3 day      = GradeWith(TESR_LUTDayBuffer, TESR_LUTData.x, lookup);
	const float3 night    = GradeWith(TESR_LUTNightBuffer, TESR_LUTData.z, lookup);
	const float3 interior = GradeWith(TESR_LUTInteriorBuffer, TESR_LUTData.w, lookup);

	// Exteriors blend day and night by the time of day; interiors use their own table. Blend.y picks between the two.
	const float3 exterior = lerp(night, day, TESR_LUTBlend.x);
	const float3 graded = lerp(exterior, interior, TESR_LUTBlend.y) * scale;

	return float4(lerp(original, graded, TESR_LUTData.y), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LUT();
	}
}
