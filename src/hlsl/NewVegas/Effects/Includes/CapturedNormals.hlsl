// UNOFFICIAL 2026-10-28 CapturedNormals, the readers' side (core/CapturedNormals.h, docs/next/CAPTURED-NORMALS.md). While the world's main
// opaque list draws, NVR's lit templates write their smooth vertex normal (world space, before the normal map) into
// TESR_CapturedNormalsBuffer: rg octahedral, b the camera distance, a 1 where written (Shaders/Includes/CapturedNormals.hlsl).
// Declare first, in your samplers' order (they bind by declaration order):
//   sampler2D TESR_CapturedNormalsBuffer : register(sN) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// CapturedNormal(uv, distance, n): true and the normal where this pixel's surface wrote one and its distance matches the depth buffer's
// (distance = length of the pixel's camera vector, toWorld(uv) * readDepth(uv)); false anywhere else (the game's own shaders, see-through
// and blended things, water, grass, decals and particles drawn later, MSAA edges, the first person): keep the depth-rebuilt normal there.
// uv: rect coordinates, as every full-screen pass's (RenderScale v2 frames: the world is drawn into the same top-left part).
#ifndef NVR_CAPTURED_NORMALS_READ_HLSL
#define NVR_CAPTURED_NORMALS_READ_HLSL

float4 TESR_CapturedNormalsData;	// x: 1 = holds this frame's normals, y: the distance match's share of the distance, z: and units, w: CapturedNormalAgrees' least cosine

float3 CapturedDecodeNormal(float2 e)
{
	float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
	return normalize(n);
}

bool CapturedNormal(float2 uv, float distance, out float3 n)
{
	n = float3(0.0f, 0.0f, 1.0f);
	[branch] if (TESR_CapturedNormalsData.x <= 0.0f) return false;
	float4 c = tex2Dlod(TESR_CapturedNormalsBuffer, float4(uv, 0.0f, 0.0f));
	n = CapturedDecodeNormal(c.rg);
	return c.a > 0.99f && abs(c.b - distance) <= distance * TESR_CapturedNormalsData.y + TESR_CapturedNormalsData.z;
}

// True where this pixel is an actor's (the skin, skinned armour and clothes: they write alpha 2) and the distance matches.
bool CapturedActor(float2 uv, float distance)
{
	[branch] if (TESR_CapturedNormalsData.x <= 0.0f) return false;
	float4 c = tex2Dlod(TESR_CapturedNormalsBuffer, float4(uv, 0.0f, 0.0f));
	return c.a > 1.5f && abs(c.b - distance) <= distance * TESR_CapturedNormalsData.y + TESR_CapturedNormalsData.z;
}

// As CapturedNormal, and only where the captured normal is within about 60 degrees of the depth-rebuilt one (rebuilt: world space, unit;
// cosine TESR_CapturedNormalsData.w). A smooth normal differs from its triangle's flat one by far less on any real mesh; the first bench
// run (2026-10-28, og-desert) showed a few thin pieces at a slab's edge whose captured normal was 90+ degrees off (not found yet which
// shader draws them): those keep the depth-rebuilt normal. Use this one in effects.
bool CapturedNormalAgrees(float2 uv, float distance, float3 rebuilt, out float3 n)
{
	return CapturedNormal(uv, distance, n) && dot(n, rebuilt) > TESR_CapturedNormalsData.w;
}

#endif
