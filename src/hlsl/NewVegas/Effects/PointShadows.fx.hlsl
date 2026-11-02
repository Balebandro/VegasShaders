// Lamp shadow mask: how much of the nearby point lights and the spot light reach each visible surface.
// Written from specs/PointShadows.md (wave 3, 2026-11-02). Techniques by index (the engine asks for them by index and name):
//   0 MergedPointShadows  full mask (cube-mapped lamps 0..10, unshadowed lamp 11, tracked lamps, spot, extra lamps)
//   1 AddShadowedLamps    the outdoor batch: cube-mapped lamps 0..10 only, added (additive blend)
//   2 LampContact         screen-space contact multiplier (multiply blend), indoors
//   3 LampMaskHalf        the mask at half resolution, G = full-resolution depth (into NVR_LampMaskHalf)
//   4 LampMaskUpsample    depth-aware upsample of NVR_LampMaskHalf into the buffer
// Output is grey (R = G = B); the mask is saturated to 0..1 because the buffer is G16R16 (unsigned).

// --- constants (names and sizes as the C++ fills them) ---
float4 TESR_ShadowLightPosition[12];	// xyz lamp position, w radius (slot 11 has no cube map)
float4 TESR_LightPosition[12];			// tracked lamps: xyz position, w radius (w 0 ends the list)
float4 TESR_LightColor[24];				// 0..11: shadow slots, 12..23: tracked lamps. rgb colour, w dimmer
float4 TESR_ExtraLights[16];			// xyz position, w = floor(w / 8) radius, w - 8 radius weight (w 0 ends the list)
float4 TESR_ShadowFade;					// z: cube-map switch (1 = read the cube maps)
float4 TESR_PointShadowNear;			// x NearFade, y EdgeFade, z reach value (>0 reach mode, <0 forward mode 2), w MaskSkipFar
float4 TESR_SpotLightPosition;			// xyz, w radius
float4 TESR_SpotLightDirection;			// xyz direction, w full cone angle (degrees)
float4 TESR_SpotLightColor;				// rgb colour, w dimmer
float4 TESR_LampContact;				// x strength, y ray length, z thickness, w normal offset
float4 TESR_LampContactFade;			// x fade-in start, y 1 / fade-in range, z fade-out end, w 1 / fade-out range
float4 TESR_LampContactBias;			// x depth bias, y bias per unit depth, z normal offset per unit depth, w minimum lamp share
float4 TESR_LampContactNoise;			// xy noise factors, z 52.98, w 1 = noise on
float4 TESR_ReciprocalResolution;		// 1 / full width, 1 / full height

// --- samplers (register numbers as the spec: depth s0, normals s1, cube maps s2..s12, the half mask s13) ---
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer4 : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer5 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer6 : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer7 : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer8 : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer9 : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer10 : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// The half-resolution mask (technique 3 writes it, technique 4 reads it). Bound by the C++ on stage 13. The spec's open question 1:
// the effect file says CLAMP; the Vulkan upsample says WRAP. CLAMP kept (the file is the reference). Only the screen edge differs.
sampler2D NVR_LampMaskHalf : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

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

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float LumaOfColour(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// The light one lamp gives at a surface point, before any shadow: 0 at or beyond its radius (a radius of 0 gives an infinite or NaN
// distance, which the test below rejects). Normal mode: the radius falloff times the surface-angle term. Reach mode (value z > 0): the
// game radius counts in full and fades over the rest of the reach, no angle term. Forward mode 2 (z < 0): falloff only.
float LampFactor(float3 pos, float3 n, float3 lamp, float radius)
{
	float3 toLamp = lamp - pos;
	float d = length(toLamp) / radius;
	if (!(d < 1.0f)) return 0.0f;
	float reach = TESR_PointShadowNear.z;
	float f;
	if (reach > 0.0f) {
		f = 1.0f - smoothstep(reach, 1.0f, d);
	} else {
		float s = d * d;
		f = (1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s);
		if (reach == 0.0f) {
			float3 dir = toLamp / max(length(toLamp), 1e-6f);
			f *= lerp(1.0f, dot(dir, n), smoothstep(0.0f, 0.2f, d));
		}
	}
	// EdgeFade: the outer share of the lamp's radius fades to zero instead of ending on a hard circle (0 = off).
	float edge = TESR_PointShadowNear.y;
	if (edge > 0.0f) f *= 1.0f - smoothstep(1.0f - edge, 1.0f, d);
	return saturate(f);
}

// 1 where the lamp reaches the surface, 0 where a caster stands between them (cube map slot with a shadow), smoothly blended near the lamp
// when NearFade is set (blockers within NearFade of the lamp cast less shadow). The cube map holds the nearest caster's distance over the
// radius per direction; 0 and 1 mean nothing was drawn there.
float LampLit(samplerCUBE cube, float3 pos, float3 lamp, float radius)
{
	if (TESR_ShadowFade.z < 0.5f) return 1.0f;
	float3 toLamp = lamp - pos;
	float d = length(toLamp) / radius;
	// Read direction: x and y negated, z kept (as the cube maps were written).
	float3 dir = float3(-toLamp.x, -toLamp.y, toLamp.z);
	float stored = texCUBElod(cube, float4(dir, 0.0f)).r;
	float lit = 1.0f;
	if (stored > 0.0f && stored < 1.0f) lit = (stored + 0.018f * d > d) ? 1.0f : 0.0f;
	if (TESR_PointShadowNear.x > 0.0f) lit = lerp(1.0f, lit, smoothstep(0.0f, TESR_PointShadowNear.x / radius, stored));
	return lit;
}

// One cube-mapped lamp slot's share of the mask: its factor, times its shadow, times its weight (luma x dimmer), saturated.
float CubeLampShare(samplerCUBE cube, int slot, float3 pos, float3 n)
{
	float4 lp = TESR_ShadowLightPosition[slot];
	float4 col = TESR_LightColor[slot];
	float f = LampFactor(pos, n, lp.xyz, lp.w);
	if (f <= 0.0f) return 0.0f;
	return saturate(f * LampLit(cube, pos, lp.xyz, lp.w) * LumaOfColour(col.rgb) * col.w);
}

// The spot light (merged technique only): radius falloff, the cone (inverse lerp between the cone limits, squared), the surface-angle
// term. Never shadowed.
float SpotShare(float3 pos, float3 n)
{
	float radius = TESR_SpotLightPosition.w;
	if (!(radius > 0.0f)) return 0.0f;
	float3 toSpot = TESR_SpotLightPosition.xyz - pos;
	float len = max(length(toSpot), 1e-6f);
	float d = len / radius;
	if (!(d < 1.0f)) return 0.0f;
	float s = d * d;
	float atten = saturate((1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s));
	float cosFull = cos(radians(TESR_SpotLightDirection.w));
	float cosHalf = cos(radians(TESR_SpotLightDirection.w * 0.5f));
	float cosAlong = saturate(dot(TESR_SpotLightDirection.xyz, -toSpot / len));
	float cone = saturate((cosAlong - cosFull) / max(cosHalf - cosFull, 1e-4f));
	cone *= cone;
	float diffuse = max(dot(toSpot / len, n), 0.0f);
	float4 col = TESR_SpotLightColor;
	return saturate(atten * cone * diffuse * LumaOfColour(col.rgb) * col.w);
}

float3 SurfacePosition(float2 uv, float depth)
{
	return TESR_CameraPosition.xyz + toWorld(uv) * depth;
}

float3 SurfaceNormalWorld(float2 uv)
{
	float3 v = mul(TESR_ViewTransform, float4(GetNormal(uv), 0.0f)).xyz;	// the rotation alone: no translation on a direction
	return v * rsqrt(max(dot(v, v), 1e-8f));
}

// The mask at one screen position: the sum of every lamp's share, saturated. batch: cube-mapped lamps 0..10 only.
float MaskAt(float2 uv, bool batch)
{
	float depth = readDepth(uv);
	float3 pos = SurfacePosition(uv, depth);
	float3 n = SurfaceNormalWorld(uv);
	float total = 0.0f;
	total += CubeLampShare(TESR_ShadowCubeMapBuffer0, 0, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer1, 1, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer2, 2, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer3, 3, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer4, 4, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer5, 5, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer6, 6, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer7, 7, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer8, 8, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer9, 9, pos, n);
	total += CubeLampShare(TESR_ShadowCubeMapBuffer10, 10, pos, n);
	if (batch) return saturate(total);

	// Slot 11: lit without a shadow lookup (no cube map).
	{
		float4 lp = TESR_ShadowLightPosition[11];
		float4 col = TESR_LightColor[11];
		total += saturate(LampFactor(pos, n, lp.xyz, lp.w) * LumaOfColour(col.rgb) * col.w);
	}
	for (int i = 0; i < 12; i++) {
		float4 lp = TESR_LightPosition[i];
		if (lp.w <= 0.0f) break;
		float4 col = TESR_LightColor[i + 12];
		total += saturate(LampFactor(pos, n, lp.xyz, lp.w) * LumaOfColour(col.rgb) * col.w);
	}
	total += SpotShare(pos, n);
	for (int e = 0; e < 16; e++) {
		if (total >= 1.0f) break;
		float4 x = TESR_ExtraLights[e];
		if (x.w <= 0.0f) break;
		float radius = floor(x.w / 8.0f);
		float weight = x.w - 8.0f * radius;
		total += saturate(LampFactor(pos, n, x.xyz, radius) * weight);
	}
	return saturate(total);
}

// Technique 0.
float4 MergedPS(VSOUT IN) : COLOR0
{
	float v = MaskAt(IN.UVCoord, false);
	return float4(v, v, v, 1.0f);
}

// Technique 1: the outdoor batch, added onto the buffer.
float4 BatchPS(VSOUT IN) : COLOR0
{
	float v = MaskAt(IN.UVCoord, true);
	return float4(v, v, v, 1.0f);
}

// Technique 3: the mask at the top-left full-resolution texel of each half-resolution pixel. G = that texel's stored depth.
// The floor is taken from the pixel's centre (+0.25 texel of the half grid), so an edge sample still lands on its own cell.
float4 HalfMaskPS(VSOUT IN) : COLOR0
{
	float2 cell = floor(IN.UVCoord / (2.0f * TESR_ReciprocalResolution.xy) + 0.25f);
	float2 uvFull = (cell * 2.0f + 0.5f) * TESR_ReciprocalResolution.xy;
	float v = MaskAt(uvFull, false);
	return float4(v, tex2Dlod(TESR_DepthBuffer, float4(uvFull, 0.0f, 0.0f)).x, 0.0f, 1.0f);
}

// Technique 2: the lamp contact multiplier, indoors. Marches toward the brightest shadowed lamp in view space: a step behind a surface
// that is nearer than the thickness blocks the lamp. 1 = no darkening; below 1 where the lamp is blocked close to the pixel.
float4 ContactPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float dist = readDepth(uv);
	float fade = saturate((dist - TESR_LampContactFade.x) * TESR_LampContactFade.y) *
		saturate((TESR_LampContactFade.z - dist) * TESR_LampContactFade.w);
	if (fade <= 0.0f) return float4(1.0f, 1.0f, 1.0f, 1.0f);

	// Lamp selection (world space): the brightest of the cube-mapped slots 0..10 by (1 - s)^2 x light, over the light of all slots 0..11.
	float3 pos = SurfacePosition(uv, dist);
	float total = 0.0f, best = 0.0f, bestRadius = 0.0f;
	float3 bestLamp = float3(0.0f, 0.0f, 0.0f);
	for (int k = 0; k < 12; k++) {
		float4 lp = TESR_ShadowLightPosition[k];
		float4 col = TESR_LightColor[k];
		if (!(lp.w > 0.0f)) continue;
		float3 t = lp.xyz - pos;
		float s2 = saturate(dot(t, t) / (lp.w * lp.w));
		float share = (1.0f - s2) * (1.0f - s2) * LumaOfColour(col.rgb) * col.w;
		total += share;
		if (k < 11 && share > best) { best = share; bestRadius = lp.w; bestLamp = lp.xyz; }
	}
	if (bestRadius <= 0.0f || best <= TESR_LampContactBias.w * total) return float4(1.0f, 1.0f, 1.0f, 1.0f);

	// View space from here on (the depth buffer, the normals and the projection are view space).
	float3 viewPos = mul(float4(toWorld(uv) * dist, 0.0f), TESR_ViewTransform).xyz;
	float3 nView = GetNormal(uv);
	float3 toLampWorld = bestLamp - pos;
	float lampDist = max(length(toLampWorld), 1e-6f);
	float3 dirView = mul(float4(toLampWorld / lampDist, 0.0f), TESR_ViewTransform).xyz;
	dirView = dirView * rsqrt(max(dot(dirView, dirView), 1e-8f));
	if (dot(dirView, nView) < 0.1f) return float4(1.0f, 1.0f, 1.0f, 1.0f);	// the surface faces away from the lamp

	float marchLength = min(TESR_LampContact.y, 0.5f * lampDist);
	float3 start = viewPos + nView * (TESR_LampContact.w + TESR_LampContactBias.z * dist);
	float ign = frac(TESR_LampContactNoise.z * frac(dot(uv, TESR_LampContactNoise.xy)));
	float jitter = TESR_LampContactNoise.w > 0.5f ? ign : 0.5f;

	float blocked = 0.0f;
	for (int i = 0; i < 6; i++) {
		float t = (float(i) + jitter) / 6.0f;
		float3 p = start + dirView * (marchLength * t);
		if (p.z <= 1e-3f) break;
		float3 sp = projectPosition(p);
		if (outsideRect(sp.xy)) break;
		float stored = readDepthLod(sp.xy);
		float gap = p.z - stored;
		float bias = TESR_LampContactBias.x + TESR_LampContactBias.y * p.z;
		if (gap > bias && gap < TESR_LampContact.z) blocked = max(blocked, 1.0f - t);
	}

	float shade = TESR_LampContact.x * fade * blocked * (best / max(total, 1e-6f));
	float v = saturate(1.0f - shade);
	return float4(v, v, v, 1.0f);
}

// Technique 4: the half-resolution mask upsampled depth-aware. Four nearest half texels (point samples at their centres), bilinear
// weight over 1 + 16 gap^2, gap = depth difference over (1 + 0.01 depth).
void UpsampleTap(float2 base, float2 f, float2 o, float2 halfTexel, float pixelDepth, inout float sum, inout float wsum)
{
	float2 c = base + o;
	float4 s = tex2Dlod(NVR_LampMaskHalf, float4((c + 0.5f) * halfTexel, 0.0f, 0.0f));
	float gap = abs(s.g * farZ - pixelDepth) / (1.0f + 0.01f * pixelDepth);
	float bw = lerp(1.0f - f.x, f.x, o.x) * lerp(1.0f - f.y, f.y, o.y);
	float w = bw / (1.0f + 16.0f * gap * gap);
	sum += w * s.r;
	wsum += w;
}

float4 UpsamplePS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float2 halfTexel = 2.0f * TESR_ReciprocalResolution.xy;
	float pixelDepth = readDepthLod(uv);
	float2 h = uv / halfTexel - 0.5f;
	float2 base = floor(h);
	float2 f = h - base;
	float sum = 0.0f, wsum = 0.0f;
	UpsampleTap(base, f, float2(0.0f, 0.0f), halfTexel, pixelDepth, sum, wsum);
	UpsampleTap(base, f, float2(1.0f, 0.0f), halfTexel, pixelDepth, sum, wsum);
	UpsampleTap(base, f, float2(0.0f, 1.0f), halfTexel, pixelDepth, sum, wsum);
	UpsampleTap(base, f, float2(1.0f, 1.0f), halfTexel, pixelDepth, sum, wsum);
	float v = saturate(sum / max(wsum, 1e-12f));
	return float4(v, v, v, 1.0f);
}

technique MergedPointShadows
{
	pass P0
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MergedPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique AddShadowedLamps
{
	pass P0
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BatchPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
		AlphaBlendEnable = TRUE; SrcBlend = One; DestBlend = One; BlendOp = Add;
	}
}

technique LampContact
{
	pass P0
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ContactPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
		AlphaBlendEnable = TRUE; SrcBlend = DestColor; DestBlend = Zero; BlendOp = Add;
	}
}

technique LampMaskHalf
{
	pass P0
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 HalfMaskPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique LampMaskUpsample
{
	pass P0
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 UpsamplePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}
