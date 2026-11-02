// UNOFFICIAL 2026-10-27 Persistent wetness and snow ([Shaders.WetWorld.Main] / [Shaders.SnowAccumulation.Main] Persistent, CoverMap.h):
// reading the cover map, a top-down map round the camera that keeps wetness, puddle water, snow and trodden snow per spot. The effect
// declares sampler2D TESR_CoverMapBuffer (WRAP addressing) before including this. The map wraps round (a spot's texel is its world
// position modulo the map's size), so it never needs moving; only its window (the area it holds now) follows the camera.

float4 TESR_CoverMapData;	// x: 1 / the map's size (game units), yz: the camera's place in the map (texture coordinates), w: the map's size
float4 TESR_CoverMapWindow;	// xy: the window's corner relative to the camera (units), z: the window's size, w: the width of its faded border (units)
float4 TESR_CoverMapGlobal;	// xyz: wetness, puddle water and snow of open ground (used outside the window), w: 1 when the map holds data
float4 TESR_CoverMapTrail;	// x: footprint trail radius (units), y: how hard steps tread (PersistentFootprints), z: the distance range the map keeps (units)

// The map keeps footprint trails as a distance field: 1 on the path the feet took, falling linearly to 0 at TESR_CoverMapTrail.z units
// from it. Bilinear filtering reproduces a linear distance exactly between texels, so the trail's edge is drawn here, per pixel, as a
// smooth round-brush stroke at any angle to the texels (a value per texel cut at a threshold gave a staircase / zigzag trail).
float CoverMapTrodden(float stored)
{
	float d = (1.0f - stored) * TESR_CoverMapTrail.z;
	return stored > 0.0001f ? TESR_CoverMapTrail.y * (1.0f - smoothstep(0.35f * TESR_CoverMapTrail.x, TESR_CoverMapTrail.x, d)) : 0.0f;
}

// rel: a world position relative to the camera (only xy is used). Returns wetness, puddle water, snow and the trail's distance field
// there (w: CoverMapTrodden turns it into how trodden the snow is); inside: how much of that came from the map (1 inside the window,
// fading to 0 at its border, where the open-ground values take over, and nothing is trodden).
float4 CoverMapReadAll(float2 rel, out float inside)
{
	float4 stored = tex2Dlod(TESR_CoverMapBuffer, float4(rel * TESR_CoverMapData.x + TESR_CoverMapData.yz, 0.0f, 0.0f));
	float2 fromCorner = rel - TESR_CoverMapWindow.xy;
	float2 edge = min(fromCorner, TESR_CoverMapWindow.z - fromCorner);
	inside = saturate(min(edge.x, edge.y) / max(TESR_CoverMapWindow.w, 1.0f)) * TESR_CoverMapGlobal.w;
	return float4(lerp(TESR_CoverMapGlobal.xyz, stored.rgb, inside), stored.a * inside);
}

// The same: wetness, puddle water and snow (xyz) and how much of that came from the map (w).
float4 CoverMapRead(float2 rel)
{
	float inside;
	float4 values = CoverMapReadAll(rel, inside);
	return float4(values.xyz, inside);
}
