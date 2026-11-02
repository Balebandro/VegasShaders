// Preview of the UNOFFICIAL Reflections effect (screen-space reflections, Reflections.fx.hlsl) without the game. The scene,
// the D3D helpers, SetCommon, GpuTime and the camera are glow_preview.cpp's (copied: its lines 9-378 and 717-785); the
// reflection run is at the end.
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/effects/BounceLightMath.h"

static int W = 1280, H = 720;
static float sceneScale = 1.0f; // every scene light times this (the 'night' run makes the whole image darker)
static const float NearZ = 5.0f, FarZ = 100000.0f;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAILED %s: 0x%08lX (line %d)\n", #x, (unsigned long)hr_, __LINE__); std::exit(1); } } while (0)

static float Hash(float x, float y) { float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f; return h - floorf(h); }
static float Noise(float x, float y) {
	float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
	float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
	return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
static float Fbm(float x, float y) { return 0.5f * Noise(x, y) + 0.3f * Noise(x * 2.1f, y * 2.1f) + 0.2f * Noise(x * 4.3f, y * 4.3f); }
static float ToGamma(float c) { c = c < 0 ? 0 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }
static float ToLinear(float c) { c = c < 0 ? 0 : c; return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// ---- the scenes: axis-aligned boxes with materials, lamps, emitters ----
// 0 street (two walls with neon signs, a lamp post, a pole and a car), 1 interior room (lamps, no ambient, a small neon sign,
// a flickering lamp and a swaying near object standing in for the weapon), 2 coplanar test (one flat wall with a sign painted
// on it and a wall at right angles to it), 3 flux test (a big tube sign and a small lightbox of the same total light).
static int sceneId = 0;
struct Box { D3DXVECTOR3 lo, hi; int material; };
static const std::vector<Box> StreetBoxes = {
	{ { -4000, -500, -20 }, { 4000, 6000, 0 }, 0 },     // 0 road
	{ { 250, -500, 0 }, { 420, 1400, 12 }, 1 },          // 1 pavement (raised)
	{ { -2500, 1400, 0 }, { 2500, 1460, 900 }, 2 },      // 2 facade ahead, blue sign on it
	{ { 420, -500, 0 }, { 800, 1400, 700 }, 3 },         // 3 building on the right, pink sign on it
	{ { -306, 594, 0 }, { -294, 606, 520 }, 4 },         // 4 pole
	{ { -190, 470, 0 }, { -40, 760, 85 }, 5 },           // 5 car
	{ { 194, 894, 0 }, { 206, 906, 420 }, 4 },           // 6 lamp post
	{ { 175, 875, 420 }, { 225, 925, 440 }, 6 },         // 7 lamp head (its underside glows)
};
static const std::vector<Box> CoplanarBoxes = {
	{ { -3000, -3000, -20 }, { 3000, 3000, 0 }, 10 },    // 0 floor
	{ { -1500, 1000, 0 }, { 300, 1060, 600 }, 11 },      // 1 the sign's wall (sign painted on its -y face)
	{ { 300, -200, 0 }, { 360, 1060, 600 }, 12 },        // 2 the wall at right angles (its -x face)
};
static const std::vector<Box> FluxBoxes = {
	{ { -3000, -3000, -20 }, { 3000, 3000, 0 }, 10 },    // 0 floor
	{ { -2500, 1000, 0 }, { 2500, 1060, 700 }, 13 },     // 1 wall with the big tube sign (x -600) and the small lightbox (x +600)
};
static float swayOffset = 0.0f, lampFlicker = 1.0f; // interior: the near object's sideways offset, lamp 0's brightness
static std::vector<Box> InteriorBoxes() {
	std::vector<Box> b = {
		{ { -600, -300, -20 }, { 600, 1500, 0 }, 20 },   // 0 floor
		{ { -600, -300, 400 }, { 600, 1500, 420 }, 20 }, // 1 ceiling
		{ { -660, -300, 0 }, { -600, 1500, 400 }, 21 },  // 2 left wall
		{ { 600, -300, 0 }, { 660, 1500, 400 }, 21 },    // 3 right wall
		{ { -600, 1500, 0 }, { 600, 1560, 400 }, 22 },   // 4 back wall (small cyan sign)
		{ { -350, 600, 0 }, { -200, 750, 120 }, 23 },    // 5 crate
		{ { 250, 900, 0 }, { 310, 960, 400 }, 21 },      // 6 pillar
		{ { -315, 385, 385 }, { -285, 415, 400 }, 24 },  // 7-9 lamp fittings (undersides glow)
		{ { 285, 785, 385 }, { 315, 815, 400 }, 24 },
		{ { -15, 1185, 385 }, { 15, 1215, 400 }, 24 },
	};
	b.push_back({ { 8 + swayOffset, 20, 138 }, { 22 + swayOffset, 34, 148 }, 25 }); // 10 the 'weapon' (nearer than NearFade 40)
	return b;
}
static bool barsOn = false; // B-13: thin vertical bars (fronds, a fence) 300 units in front of the street's facade
static int standoffMode = 0; // coplanar scene: 0 the sign painted on the wall; 1 three tube bars 6-10 units off it; 2 a canopy strip (no painted sign)
static std::vector<Box> SceneBoxes() {
	if (sceneId == 2 && standoffMode) {
		std::vector<Box> b = CoplanarBoxes;
		if (standoffMode == 1) for (float z : { 170.0f, 200.0f, 230.0f }) b.push_back({ { 140, 990, z - 2 }, { 260, 994, z + 2 }, 14 });
		else b.push_back({ { 100, 960, 300 }, { 300, 1000, 304 }, 14 });
		return b;
	}
	switch (sceneId) { case 1: return InteriorBoxes(); case 2: return CoplanarBoxes; case 3: return FluxBoxes; default: break; }
	std::vector<Box> b = StreetBoxes;
	if (barsOn) for (int k = 0; k < 23; k++) { const float x = -330.0f + 30.0f * k, w = k % 3 == 0 ? 2.0f : k % 3 == 1 ? 3.5f : 5.0f;
		b.push_back({ { x, 1100, 120 }, { x + w, 1104, 520 }, 26 }); }
	return b;
}

static float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
	float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
	float t = (wx * vx + wy * vy) / (vx * vx + vy * vy); t = t < 0 ? 0 : t > 1 ? 1 : t;
	float dx = wx - vx * t, dy = wy - vy * t;
	return sqrtf(dx * dx + dy * dy);
}

// Neon tubes: emission (linear) at a point on a sign, (u, v) in sign units.
static float neonGain = 6.0f; // every outdoor emitter's brightness x this: game-bright neon against the game's light test (M1)
static void SignA(float u, float v, float* e) {   // facade, u = x in [-420, 420], v = z in [330, 560]
	static const float segs[][4] = {
		{ -400, 340, 400, 340 }, { 400, 340, 400, 550 }, { 400, 550, -400, 550 }, { -400, 550, -400, 340 },           // frame
		{ -340, 520, -290, 380 }, { -290, 380, -240, 520 },                                                         // V
		{ -180, 380, -180, 520 }, { -180, 520, -110, 520 }, { -180, 450, -120, 450 }, { -180, 380, -110, 380 },     // E
		{ -20, 520, -70, 520 }, { -70, 520, -70, 380 }, { -70, 380, -20, 380 }, { -20, 380, -20, 445 }, { -20, 445, -45, 445 }, // G
		{ 30, 380, 70, 520 }, { 70, 520, 110, 380 }, { 45, 435, 95, 435 },                                          // A
		{ 230, 520, 160, 520 }, { 160, 520, 160, 450 }, { 160, 450, 230, 450 }, { 230, 450, 230, 380 }, { 230, 380, 160, 380 }, // S
		{ 280, 380, 360, 380 }, { 280, 520, 360, 520 },                                                             // bars
	};
	float d = 1e9f;
	for (auto& s : segs) d = min(d, SegmentDistance(u, v, s[0], s[1], s[2], s[3]));
	float tube = d < 5.0f ? 1.0f : 0.0f;
	e[0] = 0.12f * tube; e[1] = 0.32f * tube; e[2] = 1.6f * tube;
}
static void SignB(float u, float v, float* e) {   // right building, u = y in [450, 850], v = z in [150, 650]
	float d = 1e9f;
	d = min(d, SegmentDistance(u, v, 470, 170, 830, 170)); d = min(d, SegmentDistance(u, v, 830, 170, 830, 630));
	d = min(d, SegmentDistance(u, v, 830, 630, 470, 630)); d = min(d, SegmentDistance(u, v, 470, 630, 470, 170));
	for (int i = 0; i < 5; i++) d = min(d, SegmentDistance(u, v, 520, 240 + i * 85.0f, 780, 240 + i * 85.0f));
	float tube = d < 5.0f ? 1.0f : 0.0f;
	e[0] = 1.5f * tube; e[1] = 0.10f * tube; e[2] = 0.45f * tube;
}
// Flux test: the big sign (pink tubes, a frame and three bars, 400 x 200, centre x -600, z 350) and the small lightbox (40 x 20,
// centre x +600, z 350) filled with the same colour, so bright that it sends the same total light (FluxBoxGain, from BigSignFlux).
static void SignBig(float u, float v, float* e) {
	float d = 1e9f;
	d = min(d, SegmentDistance(u, v, -800, 250, -400, 250)); d = min(d, SegmentDistance(u, v, -400, 250, -400, 450));
	d = min(d, SegmentDistance(u, v, -400, 450, -800, 450)); d = min(d, SegmentDistance(u, v, -800, 450, -800, 250));
	for (int i = 0; i < 3; i++) d = min(d, SegmentDistance(u, v, -760, 300 + i * 50.0f, -440, 300 + i * 50.0f));
	float tube = d < 5.0f ? 1.0f : 0.0f;
	e[0] = 1.5f * tube; e[1] = 0.10f * tube; e[2] = 0.45f * tube;
}
static float FluxBoxGain = 1.0f;
static float fluxFlicker[2] = { 1.0f, 1.0f }; // B-12: the big sign's and the lightbox's brightness this frame
static void BigSignFlux() { // the big sign's light summed over a 1-unit grid (red), shared out over the lightbox's 40 x 20
	double sum = 0; float e[3];
	for (float v = 240.5f; v < 460; v += 1) for (float u = -810.5f; u < -390; u += 1) { SignBig(u, v, e); sum += e[0]; }
	FluxBoxGain = (float)(sum / (40.0 * 20.0) / 1.5);
}

struct Hit { float t; D3DXVECTOR3 n; int box; };
static bool RayBox(const D3DXVECTOR3& o, const D3DXVECTOR3& d, const Box& b, Hit& hit) {
	float tmin = -1e30f, tmax = 1e30f; int axis = -1;
	const float* O = &o.x; const float* D = &d.x; const float* L = &b.lo.x; const float* Hh = &b.hi.x;
	for (int a = 0; a < 3; a++) {
		if (fabsf(D[a]) < 1e-9f) { if (O[a] < L[a] || O[a] > Hh[a]) return false; continue; }
		float t1 = (L[a] - O[a]) / D[a], t2 = (Hh[a] - O[a]) / D[a];
		if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; }
		if (t1 > tmin) { tmin = t1; axis = a; }
		if (t2 < tmax) tmax = t2;
	}
	if (tmin > tmax || tmin <= 0 || axis < 0) return false;
	hit.t = tmin; hit.n = D3DXVECTOR3(0, 0, 0);
	(&hit.n.x)[axis] = D[axis] > 0 ? -1.0f : 1.0f;   // the face the ray enters faces it
	return true;
}

// Per full-resolution pixel: depth, normals, colour (as NVR has them), and for the measurements the world position, the box
// hit (-1 = sky) and the light it emits.
struct Scene { std::vector<float> depth, normals, colour, world, emit; std::vector<int> box; };
static Scene scene; // the last SetCamera's

// M1 (critic review 6): the outdoor light in the game's own units. The game's light test (BounceLightEffect::SpillThreshold) is
// luminance(linear(ambient + sun/moon)) of the weather's display-encoded colours; these are the values the game logged on the Strip:
// night: sky ambient 0.47 0.41 0.61 + moon 0.86 0.89 0.95 (1.897); dusk: 0.66 0.67 0.90 + sun 1.00 0.70 0.36 (2.295). The outdoor
// scenes are lit so that a white surface facing the moon (or sun) has exactly that brightness: linear(ambient + direct) per channel,
// shared between the ambient and the direct light in proportion. The neon is game-bright to match (neonGain).
static bool dayLight = false;   // dusk instead of night (the street; a low warm sun from the left, pale walls, a glint)
static const float NightAmbientGame[3] = { 0.47f, 0.41f, 0.61f }, NightMoonGame[3] = { 0.86f, 0.89f, 0.95f };
static const float DuskAmbientGame[3] = { 0.66f, 0.67f, 0.90f }, DuskSunGame[3] = { 1.00f, 0.70f, 0.36f };
static void OutdoorLight(float* ambientLin, float* directLin) {
	const float* a = dayLight ? DuskAmbientGame : NightAmbientGame; const float* d = dayLight ? DuskSunGame : NightMoonGame;
	for (int k = 0; k < 3; k++) { const float sum = ToLinear(a[k] + d[k]); ambientLin[k] = sum * a[k] / (a[k] + d[k]); directLin[k] = sum * d[k] / (a[k] + d[k]); }
}
// The scene's own light (linear luminance of a white surface), as BounceLightEffect::SpillThreshold works it out.
static float SceneLight() {
	if (sceneId == 1) { // interior: cell ambient 0 + directional (0.34, 0.25, 0.04) display-encoded, as the user's hangar log
		return 0.2126f * ToLinear(0.34f) + 0.7152f * ToLinear(0.25f) + 0.0722f * ToLinear(0.04f);
	}
	const float* a = dayLight ? DuskAmbientGame : NightAmbientGame; const float* d = dayLight ? DuskSunGame : NightMoonGame;
	return 0.2126f * ToLinear(a[0] + d[0]) + 0.7152f * ToLinear(a[1] + d[1]) + 0.0722f * ToLinear(a[2] + d[2]);
}

static Scene MakeScene(const D3DXVECTOR3& eye, const D3DXVECTOR3& F, const D3DXVECTOR3& R, const D3DXVECTOR3& U, const D3DXMATRIX& proj) {
	Scene s; s.depth.resize(W * H); s.normals.resize(W * H * 4); s.colour.resize(W * H * 4); s.world.resize(W * H * 3, 0.0f);
	s.emit.resize(W * H, 0.0f); s.box.resize(W * H, -1);
	const std::vector<Box> boxes = SceneBoxes();
	D3DXVECTOR3 moon(-0.4f, 0.3f, 0.85f); D3DXVec3Normalize(&moon, &moon);
	float moonC[3], ambient[3];
	OutdoorLight(ambient, moonC);   // the game's night (or dusk) light; the interior replaces it below
	struct Lamp { D3DXVECTOR3 p; float c[3]; float radius, scale; };
	std::vector<Lamp> lamps;
	if (sceneId == 0) lamps.push_back({ D3DXVECTOR3(200, 900, 415), { 0.9f, 0.75f, 0.5f }, 900.0f, 180.0f });
	if (dayLight) { moon = D3DXVECTOR3(-0.75f, 0.25f, 0.6f); D3DXVec3Normalize(&moon, &moon); }
	if (sceneId == 1) {
		D3DXVECTOR3 sun(0.3f, 0.5f, 0.8f); D3DXVec3Normalize(&sun, &sun); moon = sun;
		moonC[0] = ToLinear(0.34f); moonC[1] = ToLinear(0.25f); moonC[2] = ToLinear(0.04f);
		ambient[0] = ambient[1] = ambient[2] = 0.0f;
		const D3DXVECTOR3 at[3] = { { -300, 400, 370 }, { 300, 800, 370 }, { 0, 1200, 370 } };
		for (int i = 0; i < 3; i++) {
			const float k = i == 0 ? lampFlicker : 1.0f;
			lamps.push_back({ at[i], { 1.0f * k, 0.85f * k, 0.6f * k }, 750.0f, 260.0f });
		}
	}
	for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
		const float nx = (px + 0.5f) / W * 2 - 1, ny = 1 - (py + 0.5f) / H * 2;
		const D3DXVECTOR3 dir = F + R * (nx / proj._11) + U * (ny / proj._22);
		Hit best = { 1e30f }, h;
		for (int b = 0; b < (int)boxes.size(); b++) if (RayBox(eye, dir, boxes[b], h) && h.t < best.t) { best = h; best.box = b; }
		const int i = py * W + px;
		float* c = &s.colour[i * 4]; c[3] = 1;
		if (best.t > 1e29f) {   // night sky
			s.depth[i] = 1.0f;
			float v = (0.004f + 0.01f * max(0.0f, 1.0f - ny)) * (sceneId == 0 && dayLight ? 150.0f : 1.0f);
			c[0] = ToGamma(v * 0.5f); c[1] = ToGamma(v * 0.7f); c[2] = ToGamma(v * 1.6f);
			continue;
		}
		const D3DXVECTOR3 p = eye + dir * best.t, n = best.n;
		s.depth[i] = best.t / FarZ;   // dir has a forward component of 1: t is the view depth
		s.normals[i * 4 + 0] = D3DXVec3Dot(&n, &R) * 0.5f + 0.5f;
		s.normals[i * 4 + 1] = D3DXVec3Dot(&n, &U) * 0.5f + 0.5f;
		s.normals[i * 4 + 2] = D3DXVec3Dot(&n, &F) * 0.5f + 0.5f;
		s.normals[i * 4 + 3] = 1.0f;
		s.world[i * 3 + 0] = p.x; s.world[i * 3 + 1] = p.y; s.world[i * 3 + 2] = p.z; s.box[i] = best.box;
		float albedo[3] = { 0.3f, 0.3f, 0.3f }, e[3] = { 0, 0, 0 };
		const int m = boxes[best.box].material;
		const float pale = sceneId == 0 && dayLight ? 2.7f : 1.0f; // dusk/day: sun-bleached concrete and stucco
		if (m == 0) { float v = (0.05f + 0.03f * Fbm(p.x * 0.02f, p.y * 0.02f)) * pale * 1.5f; albedo[0] = albedo[1] = v; albedo[2] = v * 1.05f; }
		else if (m == 1) { float v = (0.18f + 0.06f * Fbm(p.x * 0.03f, p.y * 0.03f)) * pale; albedo[0] = v; albedo[1] = v * 0.97f; albedo[2] = v * 0.92f; }
		else if (m == 2) {
			float v = 0.22f + 0.1f * Fbm(p.x * 0.01f, p.z * 0.01f); albedo[0] = v; albedo[1] = v * 0.95f; albedo[2] = v * 0.9f;
			if (n.y < -0.5f && p.x > -420 && p.x < 420 && p.z > 330 && p.z < 560) { albedo[0] = albedo[1] = albedo[2] = 0.04f; SignA(p.x, p.z, e); for (int k = 0; k < 3; k++) e[k] *= neonGain; }
		}
		else if (m == 3) {
			float v = (0.26f + 0.08f * Fbm(p.y * 0.012f, p.z * 0.012f)) * pale; albedo[0] = v * 0.95f; albedo[1] = v; albedo[2] = v * 0.97f;
			if (n.x < -0.5f && p.y > 450 && p.y < 850 && p.z > 150 && p.z < 650) { albedo[0] = albedo[1] = albedo[2] = 0.05f; SignB(p.y, p.z, e); for (int k = 0; k < 3; k++) e[k] *= neonGain; }
			if (dayLight && n.x < -0.5f && p.y > 1150 && p.y < 1190 && p.z > 250 && p.z < 280) { e[0] = 40.0f; e[1] = 36.0f; e[2] = 30.0f; } // glint
		}
		else if (m == 4) { albedo[0] = albedo[1] = albedo[2] = 0.12f; }
		else if (m == 5) { albedo[0] = 0.25f; albedo[1] = 0.05f; albedo[2] = 0.04f; }
		else if (m == 6) { albedo[0] = albedo[1] = albedo[2] = 0.2f; if (n.z < -0.5f) { e[0] = 4.0f * neonGain; e[1] = 3.4f * neonGain; e[2] = 2.4f * neonGain; } }
		else if (m == 10) { float v = 0.12f + 0.04f * Fbm(p.x * 0.02f, p.y * 0.02f); albedo[0] = albedo[1] = albedo[2] = v; }
		else if (m == 11) {
			albedo[0] = albedo[1] = albedo[2] = 0.25f;
			if (!standoffMode && n.y < -0.5f && p.x > 140 && p.x < 260 && p.z > 160 && p.z < 240) { e[0] = 1.5f * neonGain; e[1] = 0.10f * neonGain; e[2] = 0.45f * neonGain; }
		}
		else if (m == 12) { albedo[0] = albedo[1] = albedo[2] = 0.25f; }
		else if (m == 14) { albedo[0] = albedo[1] = albedo[2] = 0.05f; e[0] = 1.5f * neonGain; e[1] = 0.10f * neonGain; e[2] = 0.45f * neonGain; } // stand-off tubes / canopy strip
		else if (m == 13) {
			albedo[0] = albedo[1] = albedo[2] = 0.25f;
			if (n.y < -0.5f && p.x > -810 && p.x < -390 && p.z > 240 && p.z < 460) { SignBig(p.x, p.z, e); for (int k = 0; k < 3; k++) e[k] *= fluxFlicker[0] * neonGain; }
			if (n.y < -0.5f && p.x > 580 && p.x < 620 && p.z > 340 && p.z < 360) { e[0] = 1.5f * FluxBoxGain * fluxFlicker[1] * neonGain; e[1] = 0.10f * FluxBoxGain * fluxFlicker[1] * neonGain; e[2] = 0.45f * FluxBoxGain * fluxFlicker[1] * neonGain; }
		}
		else if (m == 20 || m == 21) { float v = 0.35f + 0.08f * Fbm(p.x * 0.01f + p.z * 0.01f, p.y * 0.01f); albedo[0] = v; albedo[1] = v * 0.97f; albedo[2] = v * 0.93f; }
		else if (m == 22) {
			albedo[0] = albedo[1] = albedo[2] = 0.35f;
			if (n.y < -0.5f && p.x > -90 && p.x < 90 && p.z > 210 && p.z < 290) {
				float d = min(min(SegmentDistance(p.x, p.z, -80, 220, 80, 220), SegmentDistance(p.x, p.z, 80, 220, 80, 280)),
					min(SegmentDistance(p.x, p.z, 80, 280, -80, 280), SegmentDistance(p.x, p.z, -80, 280, -80, 220)));
				d = min(d, SegmentDistance(p.x, p.z, -60, 250, 60, 250));
				if (d < 4.0f) { e[0] = 0.15f; e[1] = 1.2f; e[2] = 1.4f; }
			}
		}
		else if (m == 23) { albedo[0] = 0.3f; albedo[1] = 0.25f; albedo[2] = 0.2f; }
		else if (m == 24) { albedo[0] = albedo[1] = albedo[2] = 0.3f; if (n.z < -0.5f) { e[0] = 3.0f; e[1] = 2.6f; e[2] = 2.0f; } }
		else if (m == 25) { albedo[0] = albedo[1] = albedo[2] = 0.05f; }
		else if (m == 26) { albedo[0] = 0.08f; albedo[1] = 0.12f; albedo[2] = 0.06f; } // B-13 bars (dark green fronds)
		// Direct light, as the game would render it: moon (or the cell's directional), ambient and the lamps (no shadows).
		float lightLevel[3];
		const float moonFall = max(0.0f, D3DXVec3Dot(&n, &moon));
		for (int k = 0; k < 3; k++) lightLevel[k] = ambient[k] + moonC[k] * moonFall;
		for (const Lamp& l : lamps) {
			D3DXVECTOR3 toLamp = l.p - p; float dl = D3DXVec3Length(&toLamp); toLamp /= dl;
			float fall = max(0.0f, D3DXVec3Dot(&n, &toLamp)) / (1.0f + (dl / l.scale) * (dl / l.scale)) * max(0.0f, 1.0f - dl / l.radius);
			for (int k = 0; k < 3; k++) lightLevel[k] += l.c[k] * fall;
		}
		for (int k = 0; k < 3; k++) c[k] = ToGamma((albedo[k] * lightLevel[k] + e[k]) * sceneScale);
		s.emit[i] = e[0] + e[1] + e[2];
	}
	return s;
}

// ---- D3D helpers ----
static IDirect3DDevice9* device;
struct Target { IDirect3DTexture9* tex; IDirect3DSurface9* surf; int w, h; };
static Target MakeTarget(int w, int h, D3DFORMAT format) {
	Target t = { nullptr, nullptr, w, h };
	CHECK(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t.tex, NULL));
	t.tex->GetSurfaceLevel(0, &t.surf);
	return t;
}
struct QuadVertex { float x, y, z, u, v; };
static IDirect3DVertexBuffer9* MakeQuad(int w, int h) {   // ShaderManager::CreateFrameVertex
	const float ox = 0.5f / w, oy = 0.5f / h;
	QuadVertex q[4] = { { -1, 1, 1, 0 + ox, 0 + oy }, { -1, -1, 1, 0 + ox, 1 + oy }, { 1, 1, 1, 1 + ox, 0 + oy }, { 1, -1, 1, 1 + ox, 1 + oy } };
	IDirect3DVertexBuffer9* vb; void* data;
	CHECK(device->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vb, NULL));
	vb->Lock(0, 0, &data, 0); memcpy(data, q, sizeof(q)); vb->Unlock();
	return vb;
}
static ID3DXEffect* LoadEffect(const std::string& path) {
	ID3DXEffect* e = nullptr; ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCreateEffectFromFileA(device, path.c_str(), NULL, NULL, 0, NULL, &e, &errors))) {
		std::printf("effect %s failed: %s\n", path.c_str(), errors ? (const char*)errors->GetBufferPointer() : "?");
		std::exit(1);
	}
	return e;
}
static void Vec(ID3DXEffect* e, const char* name, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); e->SetVector(name, &v); }
// A copy of BounceLight.fx.hlsl with lines replaced, written to `dir` with the includes it needs: for measuring only (what a part of
// the effect does, by taking it out). Exits if a line is not found.
static ID3DXEffect* EditedCopy(const std::string& fxDir, const std::string& dir, const std::vector<std::pair<std::string, std::string>>& edits) {
	CreateDirectoryA(dir.c_str(), NULL); CreateDirectoryA((dir + "\\Includes").c_str(), NULL);
	for (const char* inc : { "Helpers.hlsl", "Depth.hlsl", "Blur.hlsl" })
		CopyFileA((fxDir + "\\Includes\\" + inc).c_str(), (dir + "\\Includes\\" + inc).c_str(), FALSE);
	FILE* in = fopen((fxDir + "\\BounceLight.fx.hlsl").c_str(), "rb"); std::string text;
	char buffer[65536]; size_t n; while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) text.append(buffer, n); fclose(in);
	for (const auto& e : edits) {
		const size_t at = text.find(e.first);
		if (at == std::string::npos) { std::printf("edited copy: '%s' not found\n", e.first.c_str()); std::exit(1); }
		text.replace(at, e.first.size(), e.second);
	}
	FILE* out = fopen((dir + "\\BounceLight.fx.hlsl").c_str(), "wb"); fwrite(text.data(), 1, text.size(), out); fclose(out);
	return LoadEffect(dir + "\\BounceLight.fx.hlsl");
}
static std::vector<unsigned char> ReadBack(const Target& t, D3DFORMAT format) {
	IDirect3DSurface9* sys;
	CHECK(device->CreateOffscreenPlainSurface(t.w, t.h, format, D3DPOOL_SYSTEMMEM, &sys, NULL));
	CHECK(device->GetRenderTargetData(t.surf, sys));
	D3DLOCKED_RECT lr; CHECK(sys->LockRect(&lr, NULL, D3DLOCK_READONLY));
	const int bpp = format == D3DFMT_A32B32G32R32F ? 16 : format == D3DFMT_A16B16G16R16F ? 8 : 4;
	std::vector<unsigned char> out(t.w * t.h * bpp);
	for (int y = 0; y < t.h; y++) memcpy(&out[y * t.w * bpp], (char*)lr.pBits + y * lr.Pitch, t.w * bpp);
	sys->UnlockRect(); sys->Release();
	return out;
}
static std::vector<float> ReadF16(const Target& t) { // an A16B16G16R16F target as floats, 4 per texel
	auto raw = ReadBack(t, D3DFMT_A16B16G16R16F);
	std::vector<float> f(raw.size() / 2);
	D3DXFloat16To32Array(f.data(), (D3DXFLOAT16*)raw.data(), (UINT)f.size());
	return f;
}
static float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
// A grey PNG of a w x h map, value * scale (square root, so faint differences show).
static void SaveGray(const std::vector<float>& v, int w, int h, float scale, const std::string& path) {
	IDirect3DTexture9* t; CHECK(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
	D3DLOCKED_RECT lr; CHECK(t->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		float g = sqrtf(max(0.0f, min(1.0f, v[y * w + x] * scale)));
		unsigned b = (unsigned)(g * 255.0f + 0.5f);
		((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] = 0xFF000000u | (b << 16) | (b << 8) | b;
	}
	t->UnlockRect(0);
	CHECK(D3DXSaveTextureToFileA(path.c_str(), D3DXIFF_PNG, t, NULL)); t->Release();
	std::printf("wrote %s\n", path.c_str());
}

// ---- the inputs every pass shares ----
static IDirect3DTexture9 *depthTex, *normalTex, *sceneTex, *noiseTex;
static int spillDebug = 0;   // NVR_SpillDebug for RunBounce

static IDirect3DVertexBuffer9* fullQuad;
static D3DXMATRIX view, proj, invProj;

static void SetCommon(ID3DXEffect* e) {
	Vec(e, "TESR_ReciprocalResolution", 1.0f / W, 1.0f / H, (float)W / H, 0);
	Vec(e, "TESR_CameraData", NearZ, FarZ, 0, 0);
	Vec(e, "TESR_DepthConstants", 0, 0, 0, 1);
	e->SetMatrix("TESR_ProjectionTransform", &proj);
	e->SetMatrix("TESR_InvProjectionTransform", &invProj);
	e->SetMatrix("TESR_ViewTransform", &view);
	D3DXMATRIX invView; D3DXMatrixInverse(&invView, NULL, &view);
	e->SetMatrix("TESR_InvViewTransform", &invView);
}
// GPU time of `run` in ms, averaged over `count` runs (timestamp queries).
template <class F> static double GpuTime(F run, int count) {
	IDirect3DQuery9 *disjoint, *freq, *start, *end;
	CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint));
	CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq));
	CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &start));
	CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &end));
	run(); // warm up
	double total = 0; int good = 0;
	for (int i = 0; i < count; i++) {
		disjoint->Issue(D3DISSUE_BEGIN); start->Issue(D3DISSUE_END);
		run();
		end->Issue(D3DISSUE_END); disjoint->Issue(D3DISSUE_END); freq->Issue(D3DISSUE_END);
		BOOL dis = TRUE; UINT64 f = 0, a = 0, b = 0;
		while (disjoint->GetData(&dis, sizeof(dis), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (freq->GetData(&f, sizeof(f), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (start->GetData(&a, sizeof(a), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (end->GetData(&b, sizeof(b), D3DGETDATA_FLUSH) == S_FALSE) {}
		if (!dis && f) { total += double(b - a) / double(f) * 1000.0; good++; }
	}
	disjoint->Release(); freq->Release(); start->Release(); end->Release();
	return good ? total / good : -1;
}

// Camera at `eye`, turned yawDegrees to the right of +y, pitchDegrees down; NVR's view matrix layout and reversed depth. Makes
// the scene for it and uploads depth, normals and image.
static void SetCameraAt(const D3DXVECTOR3& eye, float yawDegrees, float pitchDegrees, float fovDegrees = 55.0f) {
	const float pitch = D3DXToRadian(pitchDegrees), yaw = D3DXToRadian(yawDegrees);
	D3DXVECTOR3 F(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), -sinf(pitch)), Up(0, 0, 1), R, U;
	D3DXVec3Cross(&R, &F, &Up); D3DXVec3Normalize(&R, &R);   // x right
	D3DXVec3Cross(&U, &R, &F);
	D3DXMatrixIdentity(&view);
	view._11 = R.x; view._12 = U.x; view._13 = F.x;
	view._21 = R.y; view._22 = U.y; view._23 = F.y;
	view._31 = R.z; view._32 = U.z; view._33 = F.z;
	view._41 = -D3DXVec3Dot(&R, &eye); view._42 = -D3DXVec3Dot(&U, &eye); view._43 = -D3DXVec3Dot(&F, &eye);
	const float fovY = D3DXToRadian(fovDegrees), aspect = (float)W / H;
	D3DXMatrixIdentity(&proj);
	proj._11 = 1.0f / (tanf(fovY * 0.5f) * aspect); proj._22 = 1.0f / tanf(fovY * 0.5f);
	proj._33 = -NearZ / (FarZ - NearZ); proj._34 = 1.0f; proj._43 = NearZ * FarZ / (FarZ - NearZ); proj._44 = 0.0f; // reversed depth, as Depth.hlsl expects
	D3DXMatrixInverse(&invProj, NULL, &proj);

	scene = MakeScene(eye, F, R, U, proj);
	if (!depthTex) {
		CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &depthTex, NULL));
		CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &normalTex, NULL));
		CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &sceneTex, NULL));
	}
	D3DLOCKED_RECT lr;
	CHECK(depthTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < H; y++) memcpy((char*)lr.pBits + y * lr.Pitch, &scene.depth[y * W], W * 4);
	depthTex->UnlockRect(0);
	CHECK(normalTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < H; y++) D3DXFloat32To16Array((D3DXFLOAT16*)((char*)lr.pBits + y * lr.Pitch), &scene.normals[y * W * 4], W * 4);
	normalTex->UnlockRect(0);
	CHECK(sceneTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < H; y++) D3DXFloat32To16Array((D3DXFLOAT16*)((char*)lr.pBits + y * lr.Pitch), &scene.colour[y * W * 4], W * 4);
	sceneTex->UnlockRect(0);
}
// The street camera: eye height 110, looking down the street (+y), a little up.
static void SetCamera(float yawDegrees, float pitchDegrees = -6.0f, float fovDegrees = 55.0f) {
	sceneId = 0;
	SetCameraAt(D3DXVECTOR3(-40.0f, 0.0f, 110.0f), yawDegrees, pitchDegrees, fovDegrees);
}
static void LookAt(int id, const D3DXVECTOR3& eye, const D3DXVECTOR3& at) {
	sceneId = id;
	const D3DXVECTOR3 d = at - eye;
	SetCameraAt(eye, D3DXToDegree(atan2f(d.x, d.y)), -D3DXToDegree(atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y))));
}

// ==== ssr_preview: the UNOFFICIAL Reflections effect (Reflections.fx.hlsl) on glow_preview's street, without the game ====
// Usage: build\ssr-test\ssr_preview.exe <Effects folder> <output folder> [width height]
// Runs ReflectionsEffect::RenderBuffers' passes the way Reflections.cpp drives them, several frames with history, then the
// composite; writes before / after / debug PNGs (display-encoded, clamped) and times the passes on the GPU.

struct ReflSettings { float dry, wet, rough, wetRough, rayLength, steps, thickness, temporal, blur, sky, drawDistance; };
static Target trace, rbuffer, temp, history, outFull;
static IDirect3DVertexBuffer9* halfQuad;
static IDirect3DTexture9* viewModelTex;
static unsigned reflFrame = 0;
// The surroundings cube, as Reflections.cpp makes it (RenderCube).
static const UINT CubeSize = 64, CubeLevels = 7;
static IDirect3DCubeTexture9 *capCube[2], *envCube;
static int capIndex = 0;
static bool useCube = false, cubeReset = true;

static void BindAll() {
	device->SetTexture(0, sceneTex); device->SetTexture(1, depthTex); device->SetTexture(2, normalTex); device->SetTexture(3, viewModelTex);
	device->SetTexture(4, trace.tex); device->SetTexture(5, rbuffer.tex); device->SetTexture(6, temp.tex); device->SetTexture(7, history.tex);
	device->SetTexture(8, capCube[capIndex]); device->SetTexture(9, capCube[capIndex ^ 1]); device->SetTexture(10, envCube);
}

static void FacePass(ID3DXEffect* e, const char* technique, IDirect3DSurface9* face, UINT size) {
	device->SetStreamSource(0, fullQuad, 0, sizeof(QuadVertex));
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	e->SetTechnique(technique);
	UINT n; CHECK(e->Begin(&n, 0));
	BindAll();
	D3DVIEWPORT9 vp = { 0, 0, size, size, 0, 1 };
	CHECK(device->SetRenderTarget(0, face)); CHECK(device->SetViewport(&vp));
	CHECK(e->BeginPass(0));
	CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
	e->EndPass(); e->End();
}

static void RenderCube(ID3DXEffect* e) {
	Vec(e, "TESR_ReflectionsCube2", 0.3f, 0.005f, (float)(CubeLevels - 1), 1.0f);
	// Capture into capCube[write] reading capCube[capIndex] (s8); BindAll puts capIndex at s8 and the other at s9.
	const int write = capIndex ^ 1;
	for (int f = 0; f < 6; f++) {
		IDirect3DSurface9* s; capCube[write]->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &s);
		Vec(e, "TESR_ReflectionsCube", (float)f, (float)CubeSize, 0.0f, cubeReset ? 1.0f : 0.0f);
		FacePass(e, "Capture", s, CubeSize); s->Release();
	}
	capIndex = write;   // now s9 (capIndex ^ 1) would be the old one: the filter reads the new capture at s9, so swap the binding
	for (UINT m = 0; m < CubeLevels; m++) for (int f = 0; f < 6; f++) {
		IDirect3DSurface9* s; envCube->GetCubeMapSurface((D3DCUBEMAP_FACES)f, m, &s);
		Vec(e, "TESR_ReflectionsCube", (float)f, (float)(CubeSize >> m), (float)m, 0.0f);
		capIndex ^= 1; FacePass(e, "Filter", s, CubeSize >> m); capIndex ^= 1; s->Release();
	}
	cubeReset = false;
}

static void Pass(ID3DXEffect* e, const char* technique, const Target& target, IDirect3DVertexBuffer9* quad) {
	device->SetStreamSource(0, quad, 0, sizeof(QuadVertex));
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	e->SetTechnique(technique);
	UINT n; CHECK(e->Begin(&n, 0));
	BindAll();
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)target.w, (DWORD)target.h, 0, 1 };
	CHECK(device->SetRenderTarget(0, target.surf)); CHECK(device->SetViewport(&vp));
	CHECK(e->BeginPass(0));   // its Texture[n] = NoTexture unbinds the pass's own target
	CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
	e->EndPass(); e->End();
}

static void RunReflections(ID3DXEffect* e, const ReflSettings& s, int debugView, bool historyValid) {
	reflFrame++;
	SetCommon(e);
	D3DXMATRIX invView; D3DXMatrixInverse(&invView, NULL, &view);
	Vec(e, "TESR_CameraPosition", invView._41, invView._42, invView._43, 1);
	Vec(e, "TESR_WaterSettings", -100000.0f, 0, 0, 0);
	Vec(e, "TESR_SkyColor", 0.06f, 0.08f, 0.16f, 1);
	Vec(e, "TESR_HorizonColor", 0.10f, 0.10f, 0.16f, 1);
	Vec(e, "TESR_ReflectionsData", s.dry, s.wet, s.rough, s.wetRough);
	const bool temporal = s.temporal > 0.0f;
	Vec(e, "TESR_ReflectionsData2", s.rayLength, s.steps, s.thickness, temporal ? (float)(reflFrame % 64) : 0.0f);
	Vec(e, "TESR_ReflectionsData3", temporal && historyValid ? s.temporal : 0.0f, s.blur, (float)debugView, s.sky);
	Vec(e, "TESR_ReflectionsData4", s.drawDistance, 0, 0, 0);
	if (reflFrame == 1) { for (const char* n : { "TESR_ReflectionsData", "TESR_ReflectionsData2", "TESR_ReflectionsData3", "TESR_ReflectionsData4", "TESR_ReflectionsLayout" }) { D3DXVECTOR4 v(-1, -1, -1, -1); D3DXHANDLE h = e->GetParameterByName(NULL, n); if (h) e->GetVector(h, &v); std::printf("check %s: %s %g %g %g %g\n", n, h ? "found" : "MISSING", v.x, v.y, v.z, v.w); } }
	Vec(e, "TESR_ReflectionsLayout", 1.0f / trace.w, 1.0f / trace.h, 0, 0);
	// Reflections.cpp's reprojection: camera-relative world -> last frame's view (rotation, then the camera's move in last
	// frame's axes) -> last frame's projection. The first frame of a run uses this frame's camera.
	static D3DXMATRIX lastView, lastInvView, lastProj;
	if (!historyValid) { lastView = view; lastInvView = invView; lastProj = proj; }
	D3DXMATRIX toLast; D3DXMatrixIdentity(&toLast);
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) toLast.m[i][j] = lastView.m[i][j];
	const double move[3] = { (double)invView._41 - lastInvView._41, (double)invView._42 - lastInvView._42, (double)invView._43 - lastInvView._43 };
	for (int j = 0; j < 3; j++) toLast.m[3][j] = (float)(move[0] * lastView.m[0][j] + move[1] * lastView.m[1][j] + move[2] * lastView.m[2][j]);
	D3DXMATRIX reproject; D3DXMatrixMultiply(&reproject, &toLast, &lastProj);
	e->SetMatrix("TESR_ReflectionsReproject", &reproject);
	lastView = view; lastInvView = invView; lastProj = proj;
	CHECK(device->BeginScene());
	if (useCube) RenderCube(e);
	else Vec(e, "TESR_ReflectionsCube2", 0.3f, 0.005f, (float)(CubeLevels - 1), 0.0f);
	if (temporal) {
		Pass(e, "Trace", trace, halfQuad);
		Pass(e, "Temporal", temp, halfQuad);
		CHECK(device->StretchRect(temp.surf, NULL, history.surf, NULL, D3DTEXF_NONE));
	}
	else Pass(e, "Trace", temp, halfQuad);
	Pass(e, "BlurX", trace, halfQuad);
	Pass(e, "BlurY", rbuffer, halfQuad);
	Pass(e, "Composite", outFull, fullQuad);
	device->EndScene();
}

static void SavePng(const Target& t, const std::string& path) {
	auto f = ReadF16(t);
	IDirect3DTexture9* png; CHECK(device->CreateTexture(t.w, t.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &png, NULL));
	D3DLOCKED_RECT lr; CHECK(png->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < t.h; y++) for (int x = 0; x < t.w; x++) {
		const float* c = &f[(y * t.w + x) * 4];
		auto b = [](float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (unsigned)(v * 255.0f + 0.5f); };
		((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] = 0xFF000000u | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
	}
	png->UnlockRect(0);
	CHECK(D3DXSaveTextureToFileA(path.c_str(), D3DXIFF_PNG, png, NULL)); png->Release();
	std::printf("wrote %s\n", path.c_str());
}

static void SaveScene(const std::string& path) {
	Target t = MakeTarget(W, H, D3DFMT_A16B16G16R16F);
	IDirect3DSurface9* src; sceneTex->GetSurfaceLevel(0, &src);
	// sceneTex is managed: copy through a system-memory surface.
	IDirect3DSurface9* sys; CHECK(device->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, NULL));
	D3DLOCKED_RECT a, b; CHECK(src->LockRect(&a, NULL, D3DLOCK_READONLY)); CHECK(sys->LockRect(&b, NULL, 0));
	for (int y = 0; y < H; y++) memcpy((char*)b.pBits + y * b.Pitch, (char*)a.pBits + y * a.Pitch, W * 8);
	src->UnlockRect(); sys->UnlockRect();
	CHECK(device->UpdateSurface(sys, NULL, t.surf, NULL));
	sys->Release(); src->Release();
	SavePng(t, path);
	t.surf->Release(); t.tex->Release();
}

// Mean linear luminance the composite added over the road below the facade's sign (where its mirror image should be) and
// over road far from any sign (where only the dark sky can show).
static void Measure(const char* label) {
	auto out = ReadF16(outFull);
	double under = 0, away = 0; int nu = 0, na = 0;
	for (int i = 0; i < W * H; i++) {
		if (scene.box[i] != 0) continue;   // the road
		const float x = scene.world[i * 3], y = scene.world[i * 3 + 1];
		float o[3], s[3];
		for (int k = 0; k < 3; k++) { o[k] = ToLinear(out[i * 4 + k]); s[k] = ToLinear(scene.colour[i * 4 + k]); }
		const float added = Luma(o) - Luma(s);
		if (x > -400 && x < 400 && y > 900 && y < 1400) { under += added; nu++; }
		if (x < -1200 && y > 600 && y < 1400) { away += added; na++; }
	}
	std::printf("%s: road under the sign +%.5f (n %d), road far from signs +%.5f (n %d)\n", label, nu ? under / nu : 0, nu, na ? away / na : 0, na);
}

int main(int argc, char** argv) {
	if (argc < 3) { std::puts("usage: ssr_preview <Effects folder> <output folder> [width height]"); return 2; }
	const std::string fxDir = argv[1], outDir = argv[2];
	if (argc > 4) { W = atoi(argv[3]); H = atoi(argv[4]); }
	CreateDirectoryA(outDir.c_str(), NULL);

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "SsrPreview"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("SsrPreview", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window;
	CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	const int hw = (W + 1) / 2, hh = (H + 1) / 2;
	trace = MakeTarget(hw, hh, D3DFMT_A16B16G16R16F); rbuffer = MakeTarget(hw, hh, D3DFMT_A16B16G16R16F);
	temp = MakeTarget(hw, hh, D3DFMT_A16B16G16R16F); history = MakeTarget(hw, hh, D3DFMT_A16B16G16R16F);
	outFull = MakeTarget(W, H, D3DFMT_A16B16G16R16F);
	fullQuad = MakeQuad(W, H); halfQuad = MakeQuad(hw, hh);
	for (int c = 0; c < 2; c++) CHECK(device->CreateCubeTexture(CubeSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &capCube[c], NULL));
	CHECK(device->CreateCubeTexture(CubeSize, CubeLevels, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &envCube, NULL));
	CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &viewModelTex, NULL));
	{ D3DLOCKED_RECT lr; viewModelTex->LockRect(0, &lr, NULL, 0); *(float*)lr.pBits = 1.0f; viewModelTex->UnlockRect(0); }

	ID3DXEffect* e = LoadEffect(fxDir + "\\Reflections.fx.hlsl");

	const ReflSettings wet = { 0.0f, 1.0f, 0.4f, 0.08f, 4000.0f, 24.0f, 1.0f, 0.85f, 4.0f, 1.0f, 8000.0f };
	ReflSettings sheen = wet; sheen.dry = 0.6f; sheen.wet = 0.0f;
	ReflSettings noHistory = wet; noHistory.temporal = 0.0f;

	struct Shot { const char* name; float pitch; };
	for (const Shot& shot : { Shot{ "street", -6.0f }, Shot{ "street_down", -14.0f } }) {
		SetCamera(6.0f, shot.pitch);
		SaveScene(outDir + "\\" + shot.name + "_0_before.png");
		auto run = [&](const ReflSettings& s, int debugView, int frames) {
			for (int f = 0; f < frames; f++) RunReflections(e, s, debugView, f > 0);
		};
		run(wet, 0, 12); SavePng(outFull, outDir + "\\" + shot.name + "_1_wet.png"); Measure((std::string(shot.name) + " wet").c_str());
		run(wet, 1, 12); SavePng(outFull, outDir + "\\" + shot.name + "_2_wet_reflection_only.png");
		run(wet, 2, 12); SavePng(outFull, outDir + "\\" + shot.name + "_3_wet_found.png");
		run(noHistory, 0, 1); SavePng(outFull, outDir + "\\" + shot.name + "_4_wet_one_frame_no_history.png");
		run(sheen, 0, 12); SavePng(outFull, outDir + "\\" + shot.name + "_5_dry_sheen.png"); Measure((std::string(shot.name) + " dry sheen").c_str());
	}

	// The surroundings cube: look round (yaw -90 .. +90 and up), then down the street with a low camera so many reflected
	// rays leave the screen; with and without the cube; and the cube itself (DebugView 3).
	{
		useCube = true; cubeReset = true;
		int f = 0;
		for (float yaw = -90.0f; yaw <= 90.0f; yaw += 15.0f) { SetCamera(yaw, -25.0f); RunReflections(e, wet, 0, false); f++; }
		for (float yaw = 90.0f; yaw >= -90.0f; yaw -= 15.0f) { SetCamera(yaw, -6.0f); RunReflections(e, wet, 0, false); f++; }
		SetCamera(6.0f, 4.0f);   // looking a little down: the signs above leave the top of the screen
		for (int k = 0; k < 12; k++) RunReflections(e, wet, 0, k > 0);
		SavePng(outFull, outDir + "\\street_7_down_with_cube.png");
		for (int k = 0; k < 12; k++) RunReflections(e, wet, 1, k > 0);
		SavePng(outFull, outDir + "\\street_8_down_with_cube_reflection_only.png");
		RunReflections(e, wet, 3, true);
		SavePng(outFull, outDir + "\\street_9_cube_debug.png");
		useCube = false;
		for (int k = 0; k < 12; k++) RunReflections(e, wet, 1, k > 0);
		SavePng(outFull, outDir + "\\street_10_down_no_cube_reflection_only.png");
		SaveScene(outDir + "\\street_11_down_before.png");
		useCube = true;
		std::printf("cube: %d look-round frames\n", f);
		const double cubeMs = GpuTime([&] { RunReflections(e, wet, 0, true); }, 20);
		std::printf("GPU: all reflection passes with the cube %.3f ms at %d x %d\n", cubeMs, W, H);
		useCube = false;
	}

	// Turning: 1.5 degrees a frame for 8 frames, against the same end view reached still (trails = the difference).
	for (int f = 0; f <= 8; f++) { SetCamera(18.0f - 1.5f * f); RunReflections(e, wet, 0, f > 0); }
	SavePng(outFull, outDir + "\\street_6_wet_after_turning.png");
	{ auto turned = ReadF16(outFull);
	  SetCamera(6.0f); for (int f = 0; f < 12; f++) RunReflections(e, wet, 0, f > 0);
	  auto still = ReadF16(outFull); double diff = 0; int n = 0;
	  for (int i = 0; i < W * H; i++) if (scene.box[i] == 0) { for (int k = 0; k < 3; k++) diff += fabs(ToLinear(turned[i * 4 + k]) - ToLinear(still[i * 4 + k])); n++; }
	  std::printf("turning vs still, road: mean linear difference %.5f per channel (n %d)\n", n ? diff / (3.0 * n) : 0.0, n); }

	SetCamera(6.0f);
	const double ms = GpuTime([&] { RunReflections(e, wet, 0, true); }, 20);
	std::printf("GPU: all reflection passes %.3f ms at %d x %d (wet street)\n", ms, W, H);
	return 0;
}
