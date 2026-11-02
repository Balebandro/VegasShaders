// UNOFFICIAL 2026-10-27 FitToDepth ([Shaders.ShadowsExteriors.Main] FitToDepth / FitToDepthDebug; src\core\SunDepthFit.h, which
// loads and draws this file itself: it is not one of the menu's effects). The sun cascades fitted to the depth actually on screen.
//   Reduce      : the linear depth buffer -> a grid of 8 x 8 pixel blocks. R: the block's farthest surface, G: its nearest surface
//                 (1e9 when it has none), B: 1 when it shows sky (view depth in game units, as the cascade splits).
//   Count       : drawn over the block grid inside a D3D9 occlusion query, once per candidate distance: the blocks that survive the
//                 clip are counted (TESR_SunDepthFitData.y 0: farthest surface past .x, 1: blocks with sky, 2: nearest surface before .x).
//   CascadeView : the debug view (FitToDepthDebug): 1 = each sun cascade coloured as the lit shaders pick it, 2 = the measured blocks.
// Native D3D9 and DXVK alike: no readback of a texture, the counts come back through the queries a few frames later.

uniform float4 TESR_ReciprocalResolution;   // x: 1 / width, y: 1 / height (the depth buffer is screen-sized)
uniform float4 TESR_SunDepthFitData;        // Count: x the distance (view depth), y what is counted (above), zw: 1 / the block grid's size
uniform float4 TESR_SunDepthFitInfo;        // CascadeView: x the view (1, 2), y how far the cascades reach now, z without the fit, w the nearest visible depth used
uniform float4 TESR_SunDepthFitSplits;      // CascadeView: the four split depths in use (view depth, game units)
uniform float4 TESR_SunDepthFitOriginal;    // CascadeView: the four split depths without the fit
uniform float4 TESR_ShadowNearCenter;       // xyz: centre (camera-relative world), w: radius -- as the lit shaders select the cascades
uniform float4 TESR_ShadowMiddleCenter;
uniform float4 TESR_ShadowFarCenter;
uniform float4 TESR_ShadowLodCenter;
uniform float4 TESR_ShadowForwardData;      // w: HandoverBlend (world units)

// TESR_ samplers bind in declaration order: s0, s1. The block grid is not a TESR_ texture: SunDepthFit.h sets it on stage 2.
uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D NVR_SunDepthFitBlocks : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "includes/Helpers.hlsl"
#include "includes/Depth.hlsl"

static const float BlockPixels = 8.0;	// SunDepthFit::BlockSize
static const float SkyDepth = 0.98;		// linear depth (viewZ / farZ) from which a pixel is sky, as the other effects test it
static const float NoSurface = 1.0e9;

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px) {
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

// One output texel = one 8 x 8 block of screen pixels (blocks at the right and bottom edges read the edge pixels again: the
// clamp, harmless for a farthest / nearest).
float4 ReducePS(VSOUT px, float2 vpos : VPOS) : COLOR0 {
	float2 first_v = floor(vpos) * BlockPixels + 0.5;
	float farthest_v = 0.0, nearest_v = 2.0, sky = 0.0;
	[loop] for (int y = 0; y < 8; y++) {
		[unroll] for (int x = 0; x < 8; x++) {
			// UNOFFICIAL 2026-10-27 RenderScale v2: the last blocks of a rect frame would reach past the drawn part
			float d = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn((first_v + float2(x, y)) * TESR_ReciprocalResolution.xy, TESR_ReciprocalResolution.xy), 0.0, 0.0)).x;
			bool surface_v = d > 0.0 && d < SkyDepth;
			sky = d >= SkyDepth ? 1.0 : sky;
			farthest_v = surface_v ? max(farthest_v, d) : farthest_v;
			nearest_v = surface_v ? min(nearest_v, d) : nearest_v;
		}
	}
	return float4(farthest_v * farZ, nearest_v < 1.5 ? nearest_v * farZ : NoSurface, sky, 1.0);
}

float4 CountPS(VSOUT px, float2 vpos : VPOS) : COLOR0 {
	float3 block_v = tex2Dlod(NVR_SunDepthFitBlocks, float4((floor(vpos) + 0.5) * TESR_SunDepthFitData.zw, 0.0, 0.0)).xyz;
	float mode_v = TESR_SunDepthFitData.y;
	float keep_v = mode_v < 0.5 ? block_v.x - TESR_SunDepthFitData.x : mode_v < 1.5 ? block_v.z - 0.5 : TESR_SunDepthFitData.x - block_v.y;
	clip(keep_v);	// what is left is counted by the occlusion query
	return float4(1.0, 1.0, 1.0, 1.0);
}

// The fit bar along the top: view depth on a square-root scale up to the shadow distance, coloured by cascade (red Near, green
// Middle, blue Far, yellow Lod, grey past the last); the thick upper bar is the layout in use, the thin one under it the layout
// without the fit. The nearest visible depth used is marked white on the upper bar.
float3 BarColour(float eye_range, float4 splits_v) {
	return eye_range < splits_v.x ? float3(0.9, 0.2, 0.2) : eye_range < splits_v.y ? float3(0.2, 0.85, 0.2) : eye_range < splits_v.z ? float3(0.25, 0.4, 1.0) :
		eye_range < splits_v.w ? float3(0.95, 0.9, 0.2) : float3(0.15, 0.15, 0.15);
}

float4 CascadeViewPS(VSOUT px) : COLOR0 {
	float2 uv = px.UVCoord;
	float3 scene_rgb = tex2D(TESR_RenderedBuffer, uv).rgb;
	float2 pixel_v = floor(uv / TESR_ReciprocalResolution.xy);

	if (uv.x > 0.02 && uv.x < 0.98 && uv.y > 0.012 && uv.y < 0.042) {
		float t = (uv.x - 0.02) / 0.96;
		float eye_range = t * t * max(TESR_SunDepthFitInfo.z, 1.0);
		if (uv.y > 0.03 && uv.y < 0.033) return float4(0.0, 0.0, 0.0, 1.0);
		if (uv.y < 0.03) {
			float near_mark = TESR_SunDepthFitInfo.w;
			float mark_x = 0.02 + 0.96 * sqrt(saturate(near_mark / max(TESR_SunDepthFitInfo.z, 1.0)));
			if (near_mark > 0.0 && abs(uv.x - mark_x) < TESR_ReciprocalResolution.x * 1.5) return float4(1.0, 1.0, 1.0, 1.0);
			return float4(BarColour(eye_range, TESR_SunDepthFitSplits), 1.0);
		}
		return float4(BarColour(eye_range, TESR_SunDepthFitOriginal) * 0.8, 1.0);
	}

	if (TESR_SunDepthFitInfo.x > 1.5) {
		// 2: the measured block grid. Grey: the block's farthest surface (white = the shadow distance); magenta: past the cascades'
		// reach now (allowed for one block in 8192); orange: nearer than the nearest depth used; cyan: the block shows sky.
		float2 grid_v = ceil(1.0 / (TESR_ReciprocalResolution.xy * BlockPixels));
		float2 cell_v = floor(pixel_v / BlockPixels);
		float3 block_v = tex2Dlod(NVR_SunDepthFitBlocks, float4((cell_v + 0.5) / grid_v, 0.0, 0.0)).xyz;
		float3 shown_v = sqrt(saturate(block_v.x / max(TESR_SunDepthFitInfo.z, 1.0))).xxx;
		if (block_v.x > TESR_SunDepthFitInfo.y) shown_v = magenta.rgb;
		if (block_v.y < NoSurface * 0.5 && block_v.y < TESR_SunDepthFitInfo.w) shown_v = float3(1.0, 0.5, 0.0);
		if (block_v.z > 0.5) shown_v = lerp(shown_v, cyan.rgb, 0.6);
		return float4(lerp(scene_rgb, shown_v, 0.75), 1.0);
	}

	// 1: the cascade each pixel's shadow comes from, chosen as Includes/Shadow.hlsl GetSunShadow does (the first sphere holding the
	// point); diagonal stripes where it cross-fades into the next (the hand-over band).
	float eye_range = readDepthLod(uv);
	if (eye_range <= 0.0 || eye_range >= farZ * SkyDepth) return float4(scene_rgb, 1.0);
	float3 posn = toWorld(uv) * eye_range;	// camera-relative, as the cascade centres
	float4 radii_v = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
	float4 distances_v = float4(length(posn - TESR_ShadowNearCenter.xyz), length(posn - TESR_ShadowMiddleCenter.xyz),
		length(posn - TESR_ShadowFarCenter.xyz), length(posn - TESR_ShadowLodCenter.xyz));
	float4 bands_v = max(0.1 * radii_v, min(TESR_ShadowForwardData.w, radii_v));	// Shadow.hlsl ShadowCascadeBands
	float3 tint_rgb = float3(0.45, 0.45, 0.45);	// past every cascade: no cascade shadow
	float band_v = 0.0;
	if (distances_v.x < radii_v.x) { tint_rgb = float3(1.0, 0.25, 0.25); band_v = distances_v.x > radii_v.x - bands_v.x ? 1.0 : 0.0; }
	else if (distances_v.y < radii_v.y) { tint_rgb = float3(0.25, 1.0, 0.25); band_v = distances_v.y > radii_v.y - bands_v.y ? 1.0 : 0.0; }
	else if (distances_v.z < radii_v.z) { tint_rgb = float3(0.3, 0.45, 1.0); band_v = distances_v.z > radii_v.z - bands_v.z ? 1.0 : 0.0; }
	else if (distances_v.w < radii_v.w) { tint_rgb = float3(1.0, 0.95, 0.25); band_v = distances_v.w > radii_v.w - bands_v.w ? 1.0 : 0.0; }
	float3 shown_v = tint_rgb * (0.3 + 0.7 * luma(scene_rgb));	// the picture's own light and shadow kept inside the tint
	if (band_v > 0.5 && frac((pixel_v.x + pixel_v.y) / 10.0) < 0.5) shown_v *= 0.65;
	return float4(lerp(scene_rgb, shown_v, 0.7), 1.0);
}

technique Reduce
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 ReducePS( );
		ZEnable = FALSE;
		ZWriteEnable = false;
		StencilEnable = FALSE;
		AlphaBlendEnable = FALSE;
		AlphaTestEnable = FALSE;
		ScissorTestEnable = false;
		CullMode = None;
		ColorWriteEnable = 15;
	}
}

technique Count
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 CountPS( );
		ZEnable = FALSE;
		ZWriteEnable = false;
		StencilEnable = FALSE;
		AlphaBlendEnable = FALSE;
		AlphaTestEnable = FALSE;
		ScissorTestEnable = false;
		CullMode = None;
		ColorWriteEnable = 15;
	}
}

technique CascadeView
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 CascadeViewPS( );
	}
}
