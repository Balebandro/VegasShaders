// Bloom: the HDR glow pyramid. Level 0 (TESR_BloomBuffer) is read by the game's tonemap shader, so it holds the finished glow.
// Techniques 0-7 downsample: technique t reads level t-1 (technique 0 reads the scene) and writes level t.
// Techniques 8-14 upsample: technique 14 - i adds the level above (i + 1) to level i's own downsample; technique 14 writes level 0.
// The C++ (effects/Bloom.cpp) sets the constants below by name for each pass.

float4 TESR_BloomData;        // x, y: tent radius of the upsample (uv units); z: PassBlending (0 = add); w: 1 / passes (not used here)
float4 TESR_BloomResolution;  // the level being written: xy size in texels, zw 1 / size
float4 TESR_BloomGlow;        // x: spread weight per wider level; y: scale of the last upsample; z: GlowBoost; w: GlowLightLevel x scene light
float4 TESR_BloomGlowExtra;   // x: AntiFlicker (1 or 0); y: LinearLight (1 or 0)

// Level i is register s<i>; the names carry the level's size factor 2^i (no suffix for level 0). Level 8 is the scene.
sampler2D TESR_BloomBuffer    : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer2   : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer4   : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer8   : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer16  : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer32  : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer64  : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer128 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_RenderedBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// RenderScale: a level is drawn in its top-left part; the taps are kept inside it (no-op on native frames).
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

// Linear light is approximated by a square and display encoding by its square root: an exact inverse pair, and cheap.
float3 ToLinear(float3 c) { c = max(c, 0.0f); return c * c; }
float3 ToDisplay(float3 c) { return sqrt(max(c, 0.0f)); }

// One tap of the first downsample when GlowBoost or LinearLight is on: linear light, bright taps boosted, then display again unless LinearLight.
float3 GlowTap(float3 c)
{
	c = ToLinear(c);
	const float boost = TESR_BloomGlow.z;
	if (boost > 0.0f) {
		const float threshold = max(TESR_BloomGlow.w, 0.002f);
		const float peak = max(c.r, max(c.g, c.b));
		const float low = min(c.r, min(c.g, c.b));
		const float saturation = 1.0f - low / max(peak, 1e-6f);
		const float brightness = Luma709(c) + (peak - Luma709(c)) * saturation;
		const float amount = saturate((brightness - threshold) / threshold);
		c *= 1.0f + boost * amount;
	}
	return (TESR_BloomGlowExtra.y > 0.5f) ? c : ToDisplay(c);
}

// A tap at texel offset o from uv, kept inside the drawn part of the source (texel = 1 / the source's size).
float3 Fetch(sampler2D src, float2 uv, float2 texel, float2 o, bool convert)
{
	const float3 c = tex2D(src, clampToRectIfOn(uv + o * texel, texel)).rgb;
	return convert ? GlowTap(c) : c;
}

// The 13-tap downsample: five 2 x 2 boxes (centre weight 1/2, the four corner boxes 1/8 each), so the weights sum to 1.
// First pass only: GlowBoost / LinearLight on the taps, and AntiFlicker weighing the boxes.
float3 Downsample(sampler2D src, float2 uv, bool first)
{
	const float2 texel = 0.5f * TESR_BloomResolution.zw;  // the source level is twice the size of the level written
	const bool convert = first && (TESR_BloomGlow.z > 0.0f || TESR_BloomGlowExtra.y > 0.5f);

	const float3 a = Fetch(src, uv, texel, float2(-2.0f,  2.0f), convert);
	const float3 b = Fetch(src, uv, texel, float2( 0.0f,  2.0f), convert);
	const float3 c = Fetch(src, uv, texel, float2( 2.0f,  2.0f), convert);
	const float3 d = Fetch(src, uv, texel, float2(-2.0f,  0.0f), convert);
	const float3 e = Fetch(src, uv, texel, float2( 0.0f,  0.0f), convert);
	const float3 f = Fetch(src, uv, texel, float2( 2.0f,  0.0f), convert);
	const float3 g = Fetch(src, uv, texel, float2(-2.0f, -2.0f), convert);
	const float3 h = Fetch(src, uv, texel, float2( 0.0f, -2.0f), convert);
	const float3 i = Fetch(src, uv, texel, float2( 2.0f, -2.0f), convert);
	const float3 j = Fetch(src, uv, texel, float2(-1.0f,  1.0f), convert);
	const float3 k = Fetch(src, uv, texel, float2( 1.0f,  1.0f), convert);
	const float3 l = Fetch(src, uv, texel, float2(-1.0f, -1.0f), convert);
	const float3 m = Fetch(src, uv, texel, float2( 1.0f, -1.0f), convert);

	const float3 boxCentre = (j + k + l + m) * 0.25f;
	const float3 boxTL = (a + b + d + e) * 0.25f;
	const float3 boxTR = (b + c + e + f) * 0.25f;
	const float3 boxBL = (d + e + g + h) * 0.25f;
	const float3 boxBR = (e + f + h + i) * 0.25f;

	if (first && TESR_BloomGlowExtra.x > 0.5f) {
		// AntiFlicker: each box weighs 1 / (1 + its luma), so single hot pixels add less. Normalised by the sum of the weights.
		const float wC  = 0.5f   / (1.0f + max(Luma709(boxCentre), 0.0f));
		const float wTL = 0.125f / (1.0f + max(Luma709(boxTL), 0.0f));
		const float wTR = 0.125f / (1.0f + max(Luma709(boxTR), 0.0f));
		const float wBL = 0.125f / (1.0f + max(Luma709(boxBL), 0.0f));
		const float wBR = 0.125f / (1.0f + max(Luma709(boxBR), 0.0f));
		const float wSum = wC + wTL + wTR + wBL + wBR;
		return (boxCentre * wC + boxTL * wTL + boxTR * wTR + boxBL * wBL + boxBR * wBR) / wSum;
	}
	return 0.5f * boxCentre + 0.125f * (boxTL + boxTR + boxBL + boxBR);
}

// The 9-tap tent (centre 4, edges 2, corners 1, over 16) at radius r (uv), each tap kept inside the source's drawn part.
float3 Tent(sampler2D src, float2 uv, float2 r, float2 texel)
{
	float3 sum = 4.0f * tex2D(src, clampToRectIfOn(uv, texel)).rgb;
	sum += 2.0f * tex2D(src, clampToRectIfOn(uv + float2( r.x, 0.0f), texel)).rgb;
	sum += 2.0f * tex2D(src, clampToRectIfOn(uv + float2(-r.x, 0.0f), texel)).rgb;
	sum += 2.0f * tex2D(src, clampToRectIfOn(uv + float2(0.0f,  r.y), texel)).rgb;
	sum += 2.0f * tex2D(src, clampToRectIfOn(uv + float2(0.0f, -r.y), texel)).rgb;
	sum += tex2D(src, clampToRectIfOn(uv + float2( r.x,  r.y), texel)).rgb;
	sum += tex2D(src, clampToRectIfOn(uv + float2(-r.x,  r.y), texel)).rgb;
	sum += tex2D(src, clampToRectIfOn(uv + float2( r.x, -r.y), texel)).rgb;
	sum += tex2D(src, clampToRectIfOn(uv + float2(-r.x, -r.y), texel)).rgb;
	return sum / 16.0f;
}

// Upsample into one level. above: the level above (wider); own: this level's downsample, which is also the target in the D3D9 chain
// (the read and the write share the texture there, as the original did; see the spec's open questions). With PassBlending above 0 the
// result is a blend of own and the upsampled level above; otherwise the upsampled level is added with the spread weight.
// The last level (level 0) applies the spread scale (add mode only) and, with LinearLight, encodes back to display values.
float3 Upsample(sampler2D above, sampler2D own, float2 uv, bool last)
{
	const float2 texel = 0.5f * TESR_BloomResolution.zw;  // the level above is twice the size of this one
	const float3 up = Tent(above, uv, TESR_BloomData.xy, texel);
	const float3 self = tex2D(own, uv).rgb;
	const bool blend = TESR_BloomData.z > 0.0f;

	float3 result = blend ? lerp(self, up, TESR_BloomData.z) : self + TESR_BloomGlow.x * up;
	if (last && !blend) result *= TESR_BloomGlow.y;
	if (last && TESR_BloomGlowExtra.y > 0.5f) result = ToDisplay(result);
	return result;
}

// Pixel shaders, one per technique. Downsample technique t reads level t-1; upsample into level i reads level i+1 and level i.
float4 Down0PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_RenderedBuffer, IN.UVCoord, true), 1.0f); }
float4 Down1PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer, IN.UVCoord, false), 1.0f); }
float4 Down2PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer2, IN.UVCoord, false), 1.0f); }
float4 Down3PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer4, IN.UVCoord, false), 1.0f); }
float4 Down4PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer8, IN.UVCoord, false), 1.0f); }
float4 Down5PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer16, IN.UVCoord, false), 1.0f); }
float4 Down6PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer32, IN.UVCoord, false), 1.0f); }
float4 Down7PS(VSOUT IN) : COLOR0 { return float4(Downsample(TESR_BloomBuffer64, IN.UVCoord, false), 1.0f); }

float4 Up6PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer128, TESR_BloomBuffer64, IN.UVCoord, false), 1.0f); }
float4 Up5PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer64, TESR_BloomBuffer32, IN.UVCoord, false), 1.0f); }
float4 Up4PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer32, TESR_BloomBuffer16, IN.UVCoord, false), 1.0f); }
float4 Up3PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer16, TESR_BloomBuffer8, IN.UVCoord, false), 1.0f); }
float4 Up2PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer8, TESR_BloomBuffer4, IN.UVCoord, false), 1.0f); }
float4 Up1PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer4, TESR_BloomBuffer2, IN.UVCoord, false), 1.0f); }
float4 Up0PS(VSOUT IN) : COLOR0 { return float4(Upsample(TESR_BloomBuffer2, TESR_BloomBuffer, IN.UVCoord, true), 1.0f); }

// Technique index = the C++'s pass number (RenderBloomBuffer): 0-7 downsample, 8-14 upsample (14 - i into level i).
technique Down0  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down0PS(); } }
technique Down1  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down1PS(); } }
technique Down2  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down2PS(); } }
technique Down3  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down3PS(); } }
technique Down4  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down4PS(); } }
technique Down5  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down5PS(); } }
technique Down6  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down6PS(); } }
technique Down7  { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Down7PS(); } }
technique Up6    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up6PS(); } }
technique Up5    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up5PS(); } }
technique Up4    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up4PS(); } }
technique Up3    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up3PS(); } }
technique Up2    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up2PS(); } }
technique Up1    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up1PS(); } }
technique Up0    { pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Up0PS(); } }
