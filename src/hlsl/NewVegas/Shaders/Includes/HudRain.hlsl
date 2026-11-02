// UNOFFICIAL 2026-10-14: rain on the first-person weapon and arms ([Shaders.WetWorld.Main] HudDrops, part of Wet World: the same
// rain, the same cover test). After mrfury97's HUD raindrops (Physically-Based-Reloaded testbed, commit dc5fa758, itself after
// Screen Space Shaders' HUD raindrops by Ascii1457): drawn in the meshes' own shaders so the drops stay on the surface, placed by
// the texture coordinates scaled to game units, upward faces wetter than the sides; each drop shrinks over its life and gives
// way to the next; the surface under a drop is read magnified (its texture coordinates pushed outward from the drop's middle:
// a lens), and the drop's dome tilts the normal so the light rolls over it.
// NVR's own, unlike theirs: the drops are drawn from hashed cells (no texture: the object shaders have no free sampler), the
// wetness comes as a constant (Wet World's cover test on the GPU, read back a frame later), and the drop is lit as a lens:
// the sun focused into a bright spot on its far side (a caustic), its rim darker where it bends the light away, a sharp glint.
// Compiled in only when HudDrops is on at startup (HUD_RAIN, ShaderRecord.cpp), so that otherwise the shaders are exactly as
// they were.
#ifndef HUDRAIN_INCLUDED
#define HUDRAIN_INCLUDED

#ifndef HUD_RAIN
	#define HUD_RAIN 0
#endif
#if HUD_RAIN

// Per draw (Render.cpp HudRain::SetForDraw, first-person draws only, 0 for every other): x wetness 0-1, y the drops' threshold
// (more drops below), z their clock (seconds), w the lens strength (refraction).
float4 HudRainDraw : register(c221);
// x how far the drops tilt the normal, y the drops' gloss, z the wet surface's gloss, w a drop cell's size in game units.
float4 HudRainLook : register(c222);

static float2 hudRainOffset = 0.0f;	// texture coordinates' shift under the drops (the lens)
static float2 hudRainSlope = 0.0f;	// the drops' tilt of the normal, texture space
static float hudRainGloss = 0.0f;	// 0-1: how much glossier the surface is here
static float hudRainCover = 0.0f;	// 0-1: how much of a drop is here
static float2 hudRainLocal = 0.0f;	// position in the drop, -1..1 across it (for the lens lighting)

float HudRainHash(float2 p) {
	float3 p3 = frac(float3(p.xyx) * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

// One layer of drops (p in cells): at most one drop per cell, its life a loop of `period` seconds with its own phase; it
// appears, holds and shrinks toward its middle, then the next takes its place (another position and size). Returns
// (cover 0-1, local x, local y): local = position in the drop, -1..1.
float3 HudRainLayer(float2 p, float seed, float period, float2 down, float slide) {
	float2 cell = floor(p);
	float t = HudRainDraw.z / period + HudRainHash(cell + seed);
	float gen = floor(t);	// which drop of this cell
	float life = frac(t);
	float2 key = cell + seed + gen * 7.31f;
	float h0 = HudRainHash(key), h1 = HudRainHash(key + 17.31f), h2 = HudRainHash(key + 41.97f), h3 = HudRainHash(key + 73.13f);
	float present = h0 - HudRainDraw.y;	// > 0: this cell has a drop now
	float r = lerp(0.12f, 0.38f, h1 * h1) * saturate(1.15f - life * life);	// shrinks as it ends
	float2 centre = 0.5f + (float2(h2, h3) - 0.5f) * max(1.0f - 2.0f * r, 0.0f);
	centre += down * slide * life * life * (r > 0.25f ? 1.0f : 0.0f);	// big drops on steep faces slide a little
	float2 d = frac(p) - centre;
	float2 local = d / max(r, 1e-3f);
	float cover = present > 0.0f ? saturate((1.0f - length(local)) * 6.0f) * saturate(present * 6.0f) : 0.0f;
	return float3(cover, local);
}

// Once per pixel at the TOP LEVEL (derivatives), BEFORE the material's textures are read: they are then read at
// HudRainUV(uv). position: anything that moves with the surface; valid: false when the vertex shader gave none.
void HudRain(float2 uv, float3 position, bool valid) {
	[branch] if (HudRainDraw.x > 0.0f && valid) {
		float2 uvDx = ddx(uv), uvDy = ddy(uv);
		float3 posDx = ddx(position), posDy = ddy(position);
		float unitsPerUV = (length(posDx) + length(posDy)) / max(length(uvDx) + length(uvDy), 1e-6f);
		float tiles = exp2(round(log2(max(unitsPerUV, 1e-3f) / max(HudRainLook.w, 0.05f))));
		// Which way is down in texture space, and how much the face looks up (rain lands on the upward faces).
		float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
		float2 heightPerUV = float2(posDx.z * uvDy.y - posDy.z * uvDx.y, posDy.z * uvDx.x - posDx.z * uvDy.x) / (abs(det) > 1e-12f ? det : 1e-12f);
		float slope = length(heightPerUV);
		float2 down = slope > 1e-6f ? -heightPerUV / slope : float2(0.0f, 1.0f);
		float steep = saturate(slope / max(unitsPerUV, 1e-3f));
		float3 n = normalize(cross(posDx, posDy));
		float up = abs(n.z);	// (the winding is unknown: either side up counts; undersides are rarely seen in first person)
		float2 p = uv * tiles;
		float3 a = HudRainLayer(p, 3.0f, 9.0f, down, steep * 0.5f);
		float3 b = HudRainLayer(p * 1.87f + float2(0.23f, 0.46f), 11.0f, 5.5f, down, steep * 0.5f);
		float3 drop = a.x >= b.x ? a : b;
		float scale = a.x >= b.x ? 1.0f : 1.87f;
		drop.x *= HudRainDraw.x * saturate(up + 0.25f);
		hudRainCover = drop.x;
		hudRainLocal = drop.yz;
		// The dome: height sqrt(1 - |local|^2), its slope local / height, capped near the rim.
		float h = sqrt(saturate(1.0f - dot(drop.yz, drop.yz)));
		float2 domeSlope = drop.yz / max(h, 0.35f);
		hudRainSlope = domeSlope * drop.x * HudRainLook.x;
		// The lens: read the surface nearer the drop's middle (magnified), by the refraction strength, in texture units: at
		// strength 1 up to about half a drop's radius.
		hudRainOffset = -drop.yz * h * drop.x * HudRainDraw.w * 0.15f / (tiles * scale);
		hudRainGloss = saturate(HudRainDraw.x * HudRainLook.z * saturate(up + 0.25f) + drop.x * HudRainLook.y);
	}
}

#define HudRainUV(uv) ((uv) + hudRainOffset)
// A material texture read through the drops: shifted under them, at the unshifted coordinates' mip level (the shift jumps at
// a drop's rim, and its derivatives there would pick a blurry mip).
#define HUD_RAIN_TEX(s, uv) tex2Dgrad(s, (uv) + hudRainOffset, ddx(uv), ddy(uv))

float3 HudRainNormal(float3 normalTS) {
	return normalize(normalTS + float3(hudRainSlope, 0.0f));
}

float HudRainRoughness(float roughness) {
	return lerp(roughness, 0.05f, hudRainGloss);
}

// The drop as a lens in the sun (lightTS: the sun's direction in tangent space, from the vertex shader): the light it gathers
// focused into a bright spot on its side away from the sun, its rim darker where it bends the light away. Returns the
// factor for the surface's colour (rim) in x and the caustic's strength (times the sun's light and the surface colour) in y.
float2 HudRainLens(float3 lightTS) {
	float r = length(hudRainLocal);
	float rim = smoothstep(0.55f, 0.95f, r) * hudRainCover * 0.45f;
	float2 spot = hudRainLocal + normalize(lightTS).xy * 0.45f;	// the focus lies opposite the light
	float caustic = saturate(1.0f - length(spot) * 2.2f);
	caustic = caustic * caustic * hudRainCover * saturate(lightTS.z * 2.0f) * 1.5f;
	return float2(1.0f - rim, caustic);
}

#else
	#define HudRain(uv, position, valid)
	#define HudRainUV(uv) (uv)
	#define HUD_RAIN_TEX(s, uv) tex2D(s, uv)
	#define HudRainNormal(n) (n)
	#define HudRainRoughness(r) (r)
	#define HudRainLens(l) float2(1.0f, 0.0f)
#endif

#endif
