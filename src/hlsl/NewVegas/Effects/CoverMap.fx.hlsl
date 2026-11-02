// UNOFFICIAL 2026-10-27 Persistent wetness and snow cover (deep/fork2-wetsnow; CoverMap.h / CoverMap.cpp).
// The cover map: a top-down map round the camera (PersistentMapSize units across, PersistentMapResolution texels) in two textures
// drawn together (two render targets of one format):
//   main (TESR_CoverMapBuffer, what WetWorld and SnowAccumulation read): r = how wet the ground is, g = how full its puddles are,
//     b = how deep the snow lies, a = footprint trails as a distance field (1 on the path, 0 at TESR_CoverMapTrail.z units: the
//     readers draw the trodden, compacted snow from it per pixel, Includes/CoverMap.hlsl CoverMapTrodden);
//   aux (TESR_CoverMapAux, only this file): r = the height of the top surface (0 = never seen), g = sun (1) or shade (0) at the last
//     update, b = where the height came from (1 the ortho map, 0.5 the camera's depth).
// It is world-fixed and wraps round: a spot's texel is its world position modulo the map's size, so it never has to be moved; when
// the camera moves, only the texels whose spot left the window start again (from the open-ground values the CPU keeps). Rain and
// snow reach only the top surface at each spot: the map is that surface's state, and WetWorld / SnowAccumulation still put it only
// on pixels that ARE the top surface (their own ortho tests), so ground under a roof stays dry.
// Heights: from the ortho map (the top-down depth map the rain and snow effects use) where it holds something; it holds NO LAND
// (evidence\orthomap-LOG.md 2026-10-17: every land quad fails its multibound frustum test), only statics and trees, so over plain
// land the height comes from the camera's depth instead (a spot the camera sees, facing up). A spot whose height was never seen
// behaves as open ground (the CPU's open-ground amounts: sunlit, no drift), i.e. as the old single amount.
// Technique 0: the update (ping-pong: reads the last maps, writes the other pair). Technique 1: the debug view.
// Plain D3D9 (ps_3_0): works on native D3D9 and through DXVK alike.

float4 TESR_CoverMapUpdate;		// x: texels across (N), y: 1 / N, z: texel size (game units), w: 1 when the last maps hold data
float4 TESR_CoverMapOrigin;		// xy: the window's first texel this update (its world texel index modulo N), zw: the same at the last update
float4 TESR_CoverMapShift;		// xy: window origin now minus at the last update (texels; clamped), zw: the window's corner relative to the camera (units)
float4 TESR_CoverMapRates;		// x: wetting, y: drying in full sun, z: puddle filling, w: puddle drying in full sun (amounts since the last update)
float4 TESR_CoverMapSnowRates;	// x: snow gained, y: snow melted in full sun (since the last update), z: drifts, w: footprint press (0: no step this update)
float4 TESR_CoverMapSun;		// xyz: direction to the sun (world), w: daylight (0 night .. 1 day)
float4 TESR_CoverMapShade;		// x: wet shade factor, y: snow shade factor, z: camera height above the map's reference height, w: 1 = the ortho map is this frame's
float4 TESR_CoverMapStart;		// xyz: wetness, puddle water and snow a spot starts with when it enters the window (open ground at the last update)
float4 TESR_CoverMapFootA;		// xyz: the player's feet at the last update (relative to the camera now), w: footprint trail radius (units)
float4 TESR_CoverMapFootB;		// xyz: the player's feet now (relative to the camera), w: the trail's distance field range (units; = TESR_CoverMapTrail.z)
float4 TESR_CoverMapDebug;		// x: debug view (1: the map top-down in a corner, 2: the map painted over the world), y: corner map size (share of the screen height)
float4x4 TESR_CoverMapOrthoInverse;			// the ortho map's light space back to camera-relative world space
float4x4 TESR_ShadowCameraToLightTransformOrtho;
float4 TESR_ReciprocalResolution;

// (declaration order = the stage each is bound to: EffectRecord::SetCT)
sampler2D TESR_CoverMapBuffer : register(s0) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_OrthoMapBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_RenderedBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CoverMapAux : register(s4) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/CoverMap.hlsl"

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

static const float HeightRange = 65536.0f;	// heights are kept as a 16-bit share of this many units round the reference height (1 unit steps)
static const float HeightNone = 1.0f / 65535.0f;

float EncodeHeight(float h) { return clamp((h + TESR_CoverMapShade.z) / HeightRange + 0.5f, 2.0f * HeightNone, 1.0f); }
float DecodeHeight(float a) { return (a - 0.5f) * HeightRange - TESR_CoverMapShade.z; }	// camera-relative
float HeightKnown(float a) { return a > HeightNone ? 1.0f : 0.0f; }

// The window position (0 .. N-1) of a texel for a window whose first texel is origin.
float2 WindowPos(float2 texel, float2 origin)
{
	return texel - origin + TESR_CoverMapUpdate.x * (1.0f - step(origin, texel));
}

// 1 when the texel holds the same spot it held at the last update (and the last maps hold data), else 0.
float KeptTexel(float2 texel)
{
	float2 now = WindowPos(texel, TESR_CoverMapOrigin.xy);
	float2 before = WindowPos(texel, TESR_CoverMapOrigin.zw);
	float2 miss = abs(TESR_CoverMapShift.xy - (before - now));
	return TESR_CoverMapUpdate.w * step(max(miss.x, miss.y), 0.25f);
}

float2 WrapTexel(float2 texel)
{
	return texel - TESR_CoverMapUpdate.x * floor((texel + 0.5f) * TESR_CoverMapUpdate.y);
}

float4 FetchTexel(float2 texel)
{
	return tex2Dlod(TESR_CoverMapBuffer, float4((texel + 0.5f) * TESR_CoverMapUpdate.y, 0.0f, 0.0f));
}

float4 FetchAux(float2 texel)
{
	return tex2Dlod(TESR_CoverMapAux, float4((texel + 0.5f) * TESR_CoverMapUpdate.y, 0.0f, 0.0f));
}

// The last map's height at a neighbour 'offset' texels away (camera-relative), or 'none' where it does not know one (outside the
// window, a texel that changed spot, never seen).
float NeighbourHeight(float2 texel, float2 pos, float2 offset, float none)
{
	float2 npos = pos + offset;
	float2 ntexel = WrapTexel(texel + offset);
	float a = FetchAux(ntexel).r;
	float inside = step(0.0f, min(npos.x, npos.y)) * step(max(npos.x, npos.y), TESR_CoverMapUpdate.x - 1.0f);
	return inside * KeptTexel(ntexel) * HeightKnown(a) > 0.5f ? DecodeHeight(a) : none;
}

// The top surface's height (camera-relative) at a camera-relative xy from the ortho map, starting from a guess of it (the ortho map
// looks down at a slight slant, so the guess picks the texel; two rounds put it right). found: 0 outside the ortho map or where
// nothing was drawn (plain land: the ortho map holds none).
float OrthoTop(float2 rel, float guess, out float found)
{
	float4 o = mul(float4(rel, guess, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	o.xyz /= o.w;
	float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
	float d = tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0.0f, 0.0f)).r;
	found = step(0.002f, min(uv.x, uv.y)) * step(max(uv.x, uv.y), 0.998f) * step(1e-6f, d) * step(d, 0.99999f);
	float4 p = mul(float4(o.x, o.y, d, 1.0f), TESR_CoverMapOrthoInverse);
	return p.z / p.w;
}

// The surface the camera sees on the line of sight through the screen point of (rel, z): camera-relative position.
float3 ScreenPoint(float2 uv)
{
	return toWorld(uv) * readDepthLod(uv);
}

// The top surface's height (camera-relative) at a camera-relative xy from the camera's depth: the spot at a guessed height is put
// on the screen, the depth there gives the surface on that line of sight, and its height is the next guess (three rounds). found: the
// surface point lies on this spot (within 1.5 texels: not an occluder in front), faces up (not a wall, not an underside), is not the
// sky and not the first-person weapon (closer than 60 units).
float ScreenTop(float2 rel, float guess, out float found)
{
	found = 0.0f;
	float z = guess;
	float3 p = float3(rel, guess);
	float2 uv = float2(0.5f, 0.5f);
	float onScreen = 0.0f;
	[unroll]
	for (int i = 0; i < 3; i++)
	{
		float3 view = mul(float4(rel, z, 0.0f), TESR_ViewTransform).xyz;
		float3 projected = projectPosition(view);
		uv = projected.xy;
		onScreen = step(nearZ + 1.0f, view.z) * step(0.002f, min(uv.x, uv.y)) * step(max(uv.x, uv.y), 0.998f);	// (the last round's counts)
		p = ScreenPoint(saturate(uv));
		z = p.z;
	}
	float depth = length(p);
	float2 pixel = TESR_ReciprocalResolution.xy;
	float3 px = ScreenPoint(saturate(uv + float2(pixel.x, 0.0f))) - p;
	float3 py = ScreenPoint(saturate(uv + float2(0.0f, pixel.y))) - p;
	float3 n = cross(px, py);
	n *= dot(n, -p) < 0.0f ? -1.0f : 1.0f;	// facing the camera
	float up = n.z / max(length(n), 1e-6f);
	float match = length(p.xy - rel) < 1.5f * TESR_CoverMapUpdate.z ? 1.0f : 0.0f;
	found = onScreen * match * step(0.7f, up) * step(60.0f, depth) * step(readDepthLod(saturate(uv)), farZ * 0.98f);
	return z;
}

struct UpdateOut
{
	float4 main : COLOR0;	// wetness, puddle water, snow, trodden snow
	float4 aux : COLOR1;	// height, sun, height source
};

UpdateOut UpdatePS(VSOUT IN, float2 vpos : VPOS)
{
	const float texelSize = TESR_CoverMapUpdate.z;
	float2 texel = floor(vpos);
	float kept = KeptTexel(texel);
	float4 state = kept > 0.5f ? FetchTexel(texel) : float4(TESR_CoverMapStart.xyz, 0.0f);
	float4 aux = kept > 0.5f ? FetchAux(texel) : float4(0.0f, 1.0f, 0.0f, 0.0f);
	float2 pos = WindowPos(texel, TESR_CoverMapOrigin.xy);
	float2 rel = TESR_CoverMapShift.zw + (pos + 0.5f) * texelSize;	// this spot, relative to the camera

	// The top surface's height: the ortho map's where it has something on this spot this frame; else the camera's where it sees the
	// spot (unless the height came from the ortho map before: a roof seen from above beats the ground under it seen from the side);
	// else what the map knew.
	float known = HeightKnown(aux.r);
	float height = known > 0.5f ? DecodeHeight(aux.r) : TESR_CoverMapFootB.z;
	float source = aux.b;
	float orthoFound = 0.0f;
	[branch]
	if (TESR_CoverMapShade.w > 0.5f)
	{
		float found1, found2;
		float first = OrthoTop(rel, height, found1);
		float second = OrthoTop(rel, first, found2);
		orthoFound = found1 * found2;
		if (orthoFound > 0.5f)
		{
			height = second;
			known = 1.0f;
			source = 1.0f;
		}
	}
	[branch]
	if (orthoFound < 0.5f && source < 0.75f && length(rel) < 4000.0f)
	{
		float seen;
		float top = ScreenTop(rel, height, seen);
		if (seen > 0.5f)
		{
			height = top;
			known = 1.0f;
			source = 0.5f;
		}
	}

	// Sun or shade: a short march towards the sun over the map's own heights (12 to 384 units); a higher top surface on the way
	// shades the spot. Spots whose height is unknown count as sunlit (as open ground).
	float sunlit = 1.0f;
	float2 sunFlat = TESR_CoverMapSun.xy;
	float sunFlatLength = length(sunFlat);
	[branch]
	if (known > 0.5f && TESR_CoverMapSun.w > 0.0f && TESR_CoverMapSun.z > 0.0f && sunFlatLength > 0.02f)
	{
		float2 towards = sunFlat / sunFlatLength;
		float rise = TESR_CoverMapSun.z / sunFlatLength;	// how much the ray to the sun climbs per unit
		[unroll]
		for (int k = 0; k < 6; k++)
		{
			float dist = 12.0f * exp2((float)k);
			float2 offset = floor(towards * (dist / texelSize) + 0.5f);
			float other = NeighbourHeight(texel, pos, offset, -1e9f);
			sunlit *= other > height + dist * rise + 16.0f ? 0.0f : 1.0f;
		}
	}

	// Drifts: snow gathers faster in hollows and against walls (the neighbours stand higher), slower on ridges and edges.
	float drift = 1.0f;
	[branch]
	if (known > 0.5f && TESR_CoverMapSnowRates.x > 0.0f && TESR_CoverMapSnowRates.z > 0.0f)
	{
		float reach = max(floor(32.0f / texelSize + 0.5f), 1.0f);
		float h0 = NeighbourHeight(texel, pos, float2(reach, 0.0f), 1e9f);
		float h1 = NeighbourHeight(texel, pos, float2(-reach, 0.0f), 1e9f);
		float h2 = NeighbourHeight(texel, pos, float2(0.0f, reach), 1e9f);
		float h3 = NeighbourHeight(texel, pos, float2(0.0f, -reach), 1e9f);
		float4 h = float4(h0, h1, h2, h3);
		float4 valid = step(h, 1e8f);
		float count = dot(valid, 1.0f);
		float mean = dot(h * valid, 1.0f) / max(count, 1.0f);
		drift = count > 0.5f ? 1.0f + TESR_CoverMapSnowRates.z * clamp((mean - height) / 32.0f, -1.0f, 1.0f) : 1.0f;
	}

	float sunWet = lerp(TESR_CoverMapShade.x, 1.0f, sunlit * TESR_CoverMapSun.w);
	float sunSnow = lerp(TESR_CoverMapShade.y, 1.0f, sunlit * TESR_CoverMapSun.w);
	float wet = saturate(state.r + TESR_CoverMapRates.x - TESR_CoverMapRates.y * sunWet);
	float puddle = saturate(state.g + TESR_CoverMapRates.z - TESR_CoverMapRates.w * sunWet);
	float snow = saturate(state.b + TESR_CoverMapSnowRates.x * drift - TESR_CoverMapSnowRates.y * sunSnow);

	// Footprints: the player's path since the last update (the segment from the feet then to the feet now: a stroke with a round
	// brush, whatever the step) treads the snow, only where the feet were on this spot's top surface (not on a roof above it, not in
	// the air). Kept as a distance field (Includes/CoverMap.hlsl CoverMapTrodden): 1 on the path, falling linearly to 0 at FootB.w
	// units, the union of strokes their max. SnowAccumulation draws the trail from it per pixel (soft, round, smooth at any angle):
	// trodden snow stays snow, greyer and flatter, the ground showing only in the middle. Fresh snow fills the trail again (it
	// narrows), and no snow means no trail.
	float trodden = state.a;
	[branch]
	if (TESR_CoverMapSnowRates.w > 0.0f)
	{
		float2 a = TESR_CoverMapFootA.xy;
		float2 ab = TESR_CoverMapFootB.xy - a;
		float t = saturate(dot(rel - a, ab) / max(dot(ab, ab), 0.0001f));
		float2 q = a + ab * t;
		float feetZ = lerp(TESR_CoverMapFootA.z, TESR_CoverMapFootB.z, t);
		float stroke = saturate(1.0f - length(rel - q) / TESR_CoverMapFootB.w);
		stroke *= known > 0.5f ? 1.0f - smoothstep(40.0f, 80.0f, abs(height - feetZ)) : 1.0f;
		trodden = max(trodden, stroke);
	}
	trodden = saturate(trodden - 3.0f * TESR_CoverMapSnowRates.x) * saturate(snow * 20.0f);

	UpdateOut OUT;
	OUT.main = float4(wet, puddle, snow, trodden);
	OUT.aux = float4(known > 0.5f ? EncodeHeight(height) : 0.0f, sunlit, known > 0.5f ? source : 0.0f, 0.0f);
	return OUT;
}

// Debug view ([Shaders.WetWorld.Main] PersistentDebugView), on the finished picture. 1: the map top-down in the top-left corner, north
// up, the camera at its centre (red dot): grey = dry ground, blue = wet, cyan = puddle water, white = snow, brown = trodden snow
// (footprints), darker = in shade at the last update, dark lines = the top surface's height (a line every 256 units), diagonal
// hatching = height never seen (the spot follows the open-ground amounts), yellow frame = the window, purple = the map holds no data.
// 2: the map's values painted over the world (blue wet, cyan puddles, white snow, brown trodden; red tint outside the window, where
// the open-ground values apply).
float3 DebugColour(float4 c, float3 base)
{
	float amount = saturate(max(c.r, max(c.g, c.b)));
	float3 tint = c.r * float3(0.05f, 0.30f, 1.0f) + c.g * float3(0.0f, 0.55f, 0.55f) + c.b * float3(0.9f, 0.9f, 0.9f);
	float3 shown = base * (1.0f - 0.75f * amount) + min(tint, 1.0f) * 0.85f;
	return lerp(shown, float3(0.55f, 0.32f, 0.12f), 0.85f * saturate(c.a));
}

float4 DebugPS(VSOUT IN) : COLOR0
{
	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	float2 pixel = IN.UVCoord / TESR_ReciprocalResolution.xy;
	float side = TESR_CoverMapDebug.y / TESR_ReciprocalResolution.y;	// the corner map's side in pixels
	float2 local = (pixel - 16.0f) / side;
	if (TESR_CoverMapDebug.x > 0.5f && TESR_CoverMapDebug.x < 1.5f && local.x >= 0.0f && local.y >= 0.0f && local.x <= 1.0f && local.y <= 1.0f)
	{
		float span = TESR_CoverMapWindow.z;
		float2 rel = float2(local.x - 0.5f, 0.5f - local.y) * span;	// north up, the camera in the middle
		float2 uv = rel * TESR_CoverMapData.x + TESR_CoverMapData.yz;
		float4 c = tex2Dlod(TESR_CoverMapBuffer, float4(uv, 0.0f, 0.0f));
		c.a = CoverMapTrodden(c.a);
		float4 aux = tex2Dlod(TESR_CoverMapAux, float4(uv, 0.0f, 0.0f));
		float known = HeightKnown(aux.r);
		float3 shown = DebugColour(c, float3(0.22f, 0.22f, 0.22f));
		shown *= known > 0.5f ? lerp(0.6f, 1.0f, aux.g) : 1.0f;	// shade
		float contour = known > 0.5f && frac(aux.r * HeightRange / 256.0f) < 0.2f ? 1.0f : 0.0f;
		shown *= 1.0f - 0.45f * contour;
		float hatch = frac((pixel.x + pixel.y) / 10.0f) < 0.5f ? 1.0f : 0.0f;
		shown *= known > 0.5f ? 1.0f : 1.0f - 0.35f * hatch;	// hatching: height never seen
		float2 fromCorner = rel - TESR_CoverMapWindow.xy;
		float edge = min(min(fromCorner.x, span - fromCorner.x), min(fromCorner.y, span - fromCorner.y));
		float pixelUnits = span / side;
		shown = abs(edge) < 2.0f * pixelUnits ? float3(1.0f, 0.85f, 0.0f) : shown;
		shown = edge < -2.0f * pixelUnits ? shown * 0.35f : shown;
		shown = length((local - 0.5f) * side) < 4.0f ? float3(1.0f, 0.0f, 0.0f) : shown;
		shown = TESR_CoverMapGlobal.w > 0.5f ? shown : float3(0.5f, 0.0f, 0.5f);	// purple: the map holds no data yet
		return float4(shown, 1.0f);
	}
	if (TESR_CoverMapDebug.x > 1.5f)
	{
		float depth = readDepthLod(IN.UVCoord);
		if (depth < farZ * 0.99f)
		{
			float3 rel = toWorld(IN.UVCoord) * depth;
			float inside;
			float4 c = CoverMapReadAll(rel.xy, inside);
			c.a = CoverMapTrodden(c.a);
			float3 shown = DebugColour(c, color.rgb);
			shown = lerp(shown, shown * float3(1.0f, 0.55f, 0.55f), 1.0f - inside);
			color.rgb = lerp(color.rgb, shown, 0.75f);
		}
	}
	return color;
}

technique UpdateTechnique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 UpdatePS();
	}
}

technique DebugTechnique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DebugPS();
	}
}
