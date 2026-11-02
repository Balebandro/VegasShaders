// Preview of the UNOFFICIAL NeonSpill (BounceLight.fx.hlsl) and the bloom additions (Bloom.fx.hlsl) without the game: scenes made
// here (depth, view-space normals and a display-encoded HDR image, as NVR has them), run through the real effect files the way
// BounceLight.cpp and Bloom.cpp drive them, then a plain tonemap with NVR's bloom blend. Writes PNGs, checks that the new files at
// their "off" settings give the old files' result bit for bit (over several frames, with history), measures the neon spill's
// known problems (sign walls lit by their own spill, small vs big signs, light after a turn stops, interior flicker) and times
// the passes on the GPU.
// Usage: build\glow-test\glow_preview.exe <Effects folder> <pre-spill Effects folder> <round-5 Effects folder> <output folder>
//        [width height [sceneScale]]   (environment GLOW_PARTS=45 runs only parts 4 and 5, and so on)
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

// The [Shaders.BounceLight.Main] settings RunBounce mirrors. technique: 0 the first (all extras off), 1 BounceLightPlus,
// 2 BounceLightTemporal, 3 BounceLightTemporalHalf (BounceLight.cpp picks it from Temporal/HalfSamples).
// UNOFFICIAL round 6 keys (0 = round 5): holdAfter = NeonSpillHoldAfterTurn (seconds), pyramidFix = NeonSpillSeparateDepth,
// capScene = NeonSpillCapSceneLight, maskSpill = NeonSpillMaskSpillOnly, hardEdges = NeonSpillHardEdges.
struct BounceSettings {
	float strength, spill, reach, lightLevel; int technique;
	float offScreen = 0.0f, multiBounce = 0.0f, holdAfter = 0.0f, pyramidFix = 0.0f, capScene = 0.0f, maskSpill = 0.0f, hardEdges = 0.0f;
};
static BounceSettings UserLike(float spill) { // the user's toml now: Strength 0.5, Temporal, MultiBounce 1, OffScreenLight 1, Reach 0.25, LightLevel 3 (Far 0.5: spillShape)
	BounceSettings s = { 0.5f, spill, 0.25f, 3.0f, 2 }; s.offScreen = 1.0f; s.multiBounce = 1.0f; return s;
}
static BounceSettings Defaults() { // the defaults toml: Strength 1.5, Temporal, MultiBounce 1, OffScreenLight 1, LightLevel 1.5; NeonSpill 1
	BounceSettings s = { 1.5f, 1.0f, 1.0f, 1.5f, 2 }; s.offScreen = 1.0f; s.multiBounce = 1.0f; return s;
}
struct BounceBuffers { Target prep, low[2], history[2], spill[7], out, memory[2]; };

static BounceBuffers MakeBounceBuffers() {
	BounceBuffers b;
	const int lw = (W + 3) / 4, lh = (H + 3) / 4;  // QuarterResolution (the user's)
	b.prep = MakeTarget(lw, lh, D3DFMT_A32B32G32R32F);
	for (int i = 0; i < 2; i++) {
		b.low[i] = MakeTarget(lw, lh, D3DFMT_A16B16G16R16F);
		b.history[i] = MakeTarget(lw, lh, D3DFMT_A16B16G16R16F);
		b.memory[i] = MakeTarget(lw, lh, D3DFMT_A16B16G16R16F);
	}
	int w = 320, h = (int)ceilf(320.0f * H / W);   // BounceLightEffect::EnsureSpill
	for (int i = 0; i < 7; i++) { b.spill[i] = MakeTarget(w, h, D3DFMT_A16B16G16R16F); w = (w + 1) / 2; h = (h + 1) / 2; }
	b.out = MakeTarget(W, H, D3DFMT_A16B16G16R16F);
	return b;
}
// Every buffer to zero (the game's are made once and never cleared; two runs compared bit for bit must start alike).
static void ClearBounceBuffers(BounceBuffers& b) {
	device->ColorFill(b.prep.surf, NULL, 0);
	for (int i = 0; i < 2; i++) { device->ColorFill(b.low[i].surf, NULL, 0); device->ColorFill(b.history[i].surf, NULL, 0); device->ColorFill(b.memory[i].surf, NULL, 0); }
	for (int i = 0; i < 7; i++) device->ColorFill(b.spill[i].surf, NULL, 0);
	device->ColorFill(b.out.surf, NULL, 0);
}

static void Draw(ID3DXEffect* e, UINT pass, const Target& target) {
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)target.w, (DWORD)target.h, 0, 1 };
	CHECK(device->SetRenderTarget(0, target.surf));
	CHECK(device->SetViewport(&vp));
	CHECK(e->BeginPass(pass));
}

// What BounceLight.cpp carries from frame to frame: last frame's camera, the gather's history, the spill's memory, the timer
// of NeonSpillHoldAfterTurn.
struct BounceState { int memoryIndex = 0, historyIndex = 0; bool memoryValid = false, historyValid = false, lastValid = false;
	D3DXMATRIX lastView, lastInvView, lastProj; unsigned frame = 0; float stillSeconds = 1.0e9f; };
static BounceState state;
static float memorySeconds = 1.0f, frameSeconds = 1.0f / 60.0f, lastThreshold = 0.0f;
static float spillShape[3] = { 1.0f, 1.0f, 0.0f }; // NeonSpillNear, NeonSpillFar, NeonSpillBrightCap
static unsigned pyramidKeep = 0x7F;
static bool setSpillOff = true;   // B-15 in BounceLight.cpp (false: round 5's C++, which never set it in the first technique)
// M3 (critic review 6): the sampler states the effect applies at BeginPass for the stages BounceLight.cpp binds by hand: point on the
// bounce, prepare and history buffers (s5-s7), bilinear + clamp on the pyramid and the spill buffer (s8-s14). Checked while
// checkSamplers is set (one RunBounce of each technique); failures counted.
static bool checkSamplers = false;
static int samplerFailures = 0, samplerChecks = 0;
static void CheckSampler(int stage, bool linear, const char* where) {
	if (!checkSamplers) return;
	DWORD mag = 0, mn = 0, u = 0, v = 0;
	device->GetSamplerState(stage, D3DSAMP_MAGFILTER, &mag); device->GetSamplerState(stage, D3DSAMP_MINFILTER, &mn);
	device->GetSamplerState(stage, D3DSAMP_ADDRESSU, &u); device->GetSamplerState(stage, D3DSAMP_ADDRESSV, &v);
	const DWORD want = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	samplerChecks++;
	if (mag != want || mn != want || u != D3DTADDRESS_CLAMP || v != D3DTADDRESS_CLAMP) {
		samplerFailures++;
		std::printf("M3 FAIL %s: stage %d mag %lu min %lu address %lu %lu (want %s, clamp)\n", where, stage, mag, mn, u, v, linear ? "linear" : "point");
	}
}                 // which pyramid levels the gather sees (measurement only: the others emptied)

static const char* TechniqueName(int t) { return t == 1 ? "BounceLightPlus" : t == 2 ? "BounceLightTemporal" : "BounceLightTemporalHalf"; }

// BounceLight.cpp's Render (quarter resolution), line for line where it matters: the light pyramid and the spill's gather with
// memory first (spill on), then prepare, gather, blur x, blur y, combine into b.out. newFile: the effect has the spill (the
// pre-spill file does not). keepState: this call is the next frame of the last one (camera, history, memory); otherwise a
// still picture with no last frame.
static void RunBounce(ID3DXEffect* e, BounceBuffers& b, const BounceSettings& s, bool newFile, bool keepState = false) {
	if (!keepState) state = BounceState();
	const int lw = b.prep.w, lh = b.prep.h;
	const bool plus = s.technique != 0;                                     // cpp 272
	const bool useHistory = s.technique >= 2;                              // cpp 243-248 (Temporal: history buffers)
	const bool spill = newFile && plus && s.spill > 0;                     // cpp 311-312
	const bool continuous = state.lastValid;                               // cpp 304 (same resolution, < 0.5 s, same cell)
	D3DXMATRIX invView; D3DXMatrixInverse(&invView, NULL, &view);
	D3DXMATRIX reproject; D3DXMatrixIdentity(&reproject);
	if (continuous) BounceLightReprojection(view, invView, state.lastView, state.lastInvView, state.lastProj, &reproject); // cpp 339
	// NeonSpillHoldAfterTurn's timer: seconds since the view last moved (BounceLight.cpp, Render).
	// M7: a break in continuity (BounceLight.cpp: quarter switch, 0.5 s gap, cell change, failed frame) ends any hold; a move restarts it.
	state.stillSeconds = !continuous ? 1.0e9f : memcmp(&view, &state.lastView, sizeof(view)) != 0 ? 0.0f : state.stillSeconds + frameSeconds;
	const float holdAfterTurn = s.holdAfter > 0.0f && state.stillSeconds < s.holdAfter ? 1.0f : 0.0f;
	SetCommon(e);
	Vec(e, "TESR_BounceLightData", spill ? 1.0f : s.strength, 150.0f, 6000.0f, 1.5f);   // cpp 319
	Vec(e, "NVR_BounceLayout", (float)lw / W, (float)lh / H, 1.0f / lw, 1.0f / lh);     // cpp 323
	device->SetStreamSource(0, fullQuad, 0, sizeof(QuadVertex));
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	CHECK(device->BeginScene());
	auto bindCommon = [&] {   // EffectRecord::SetCT's TESR_ samplers (declaration order) and the scene at stage 2 (cpp 171, 413)
		device->SetTexture(0, sceneTex); device->SetTexture(1, depthTex); device->SetTexture(2, sceneTex);
		device->SetTexture(3, normalTex); device->SetTexture(4, noiseTex);
	};
	UINT passes;
	if (spill) {   // cpp 136-179 RenderSpillPyramid
		lastThreshold = s.lightLevel * SceneLight();                       // cpp 115-131 SpillThreshold
		Vec(e, "NVR_SpillMemory", 0, 0, 0, lastThreshold);                 // cpp 147-148
		Vec(e, "NVR_SpillShape", spillShape[0], spillShape[1], s.capScene > 0.0f ? spillShape[2] / s.lightLevel : spillShape[2], 0.0f); // cpp 150-153 (+ NeonSpillCapSceneLight)
		e->SetTechnique("BounceSpillPyramid");
		CHECK(e->Begin(&passes, 0));
		for (int i = 0; i < 7; i++) {
			const Target& t = b.spill[i];
			Vec(e, "NVR_SpillTarget", 1.0f / t.w, 1.0f / t.h, i ? 1.0f / b.spill[i - 1].w : 0.0f, i ? 1.0f / b.spill[i - 1].h : 0.0f); // cpp 162-163
			for (int k = 0; k < 7; k++) device->SetTexture(8 + k, nullptr);  // cpp 164
			Draw(e, i, t);
			if (i) CheckSampler(8 + i - 1, true, "pyramid level");
			bindCommon();
			if (i) device->SetTexture(8 + i - 1, b.spill[i - 1].tex);       // cpp 170
			CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
			e->EndPass();
		}
		e->End();
		for (int k = 0; k < 7; k++) device->SetTexture(8 + k, nullptr);      // cpp 177
		for (int k = 0; k < 7; k++) if (!(pyramidKeep & (1u << k))) device->ColorFill(b.spill[k].surf, NULL, D3DCOLOR_ARGB(255, 0, 0, 0)); // (measurement)
	}
	// cpp Render: with the spill running and NeonSpillHardEdges, the technique's HardEdges variant.
	const std::string mainTechnique = plus ? std::string(TechniqueName(s.technique)) + (spill && s.hardEdges > 0.0f ? "HardEdges" : "") : std::string();
	e->SetTechnique(plus ? e->GetTechniqueByName(mainTechnique.c_str()) : e->GetTechnique(0));
	const int writeIndex = state.historyIndex ^ 1;                         // cpp 325
	if (plus) {   // cpp 326-346
		Vec(e, "NVR_BounceTemporal", useHistory && continuous && state.historyValid ? 0.9f : 0.0f, continuous ? s.multiBounce : 0.0f, (float)(state.frame % 1024), 0);
		float amb[3] = { 0.0f, 0.0f, 0.0f };   // cpp 347-351: 0.25 x BounceLinear(ambient); the interior's cell ambient is 0
		if (sceneId != 1) for (int k = 0; k < 3; k++) amb[k] = ToLinear(min(1.0f, (dayLight ? DuskAmbientGame : NightAmbientGame)[k]));
		Vec(e, "NVR_BounceAmbient", 0.25f * amb[0], 0.25f * amb[1], 0.25f * amb[2], s.offScreen);
		e->SetMatrix("NVR_BounceReproject", &reproject);
		if (newFile) Vec(e, "NVR_BounceSpill", spill ? s.spill : 0.0f, spill ? s.strength : 1.0f, s.reach, s.lightLevel);
	}
	else if (newFile && setSpillOff) Vec(e, "NVR_BounceSpill", 0.0f, 1.0f, s.reach, s.lightLevel);   // cpp B-15: the "off" value every frame
	const int memoryWrite = state.memoryIndex ^ 1;
	if (spill) {   // cpp 351-369 and 184-211 RenderSpillGather
		const bool memory = continuous && state.memoryValid;
		Vec(e, "NVR_SpillMemory", memory ? 0.85f : 0.0f, memory && memorySeconds > 0 ? expf(-frameSeconds / memorySeconds) : 0.0f,
			memory ? holdAfterTurn : 0.0f, lastThreshold);
		// cpp Render: NVR_SpillOptions = level 0's size, NeonSpillMaskSpillOnly; RenderSpillGather: NeonSpillSeparateDepth picks the technique.
		Vec(e, "NVR_SpillOptions", (float)b.spill[0].w, (float)b.spill[0].h, s.maskSpill > 0.0f ? 1.0f : 0.0f, s.maskSpill > 0.0f ? 1.5f * lastThreshold / s.lightLevel : 0.0f);
		e->SetTechnique(s.pyramidFix > 0.0f ? "BounceSpillGatherSeparate" : "BounceSpillGather");
		CHECK(e->Begin(&passes, 0));
		Draw(e, 0, b.memory[memoryWrite]);
		for (int k = 0; k < 7; k++) CheckSampler(8 + k, true, "spill gather");
		CheckSampler(7, false, "spill gather");
		bindCommon();
		for (int k = 0; k < 7; k++) device->SetTexture(8 + k, b.spill[k].tex);   // cpp 201
		device->SetTexture(7, b.memory[state.memoryIndex].tex);                   // cpp 202
		CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
		e->EndPass(); e->End();
		for (int k = 0; k < 7; k++) device->SetTexture(8 + k, nullptr);         // cpp 208-209
		device->SetTexture(7, nullptr);
		e->SetTechnique(e->GetTechniqueByName(mainTechnique.c_str()));            // cpp 367
	}
	if (newFile) Vec(e, "NVR_SpillDebug", spill ? (float)spillDebug : 0.0f, 0, 0, 0);   // cpp 372-376
	CHECK(e->Begin(&passes, 0));
	for (UINT p = 0; p < 5; p++) {   // cpp 387-420, the pass loop's binding chain
		const bool combine = p == 4;
		const Target& t = combine ? b.out : p == 0 ? b.prep : p == 1 && useHistory ? b.history[writeIndex] : b.low[p == 2 ? 1 : 0]; // cpp 393-394
		device->SetTexture(5, nullptr); device->SetTexture(6, nullptr);   // cpp 395-396
		if (plus) device->SetTexture(7, nullptr);                          // cpp 397
		Draw(e, p, t);
		if (plus && p == 0) CheckSampler(7, false, "prepare");
		if (p == 1) { CheckSampler(6, false, "gather"); if (plus) CheckSampler(7, false, "gather"); if (spill) CheckSampler(8, true, "gather"); }
		if (p >= 2) CheckSampler(5, false, p == 4 ? "combine" : "blur");
		bindCommon();
		if (p == 0 && plus) device->SetTexture(7, b.low[0].tex);           // cpp 402: last frame's final result (MultiBounce)
		if (p == 1) {
			device->SetTexture(6, b.prep.tex);                             // cpp 404
			if (useHistory) device->SetTexture(7, b.history[state.historyIndex].tex); // cpp 405: last frame's gather
		}
		else if (p == 2 && useHistory) device->SetTexture(5, b.history[writeIndex].tex); // cpp 407
		else if (p == 2 || p == 4) device->SetTexture(5, b.low[0].tex);   // cpp 408
		else if (p == 3) device->SetTexture(5, b.low[1].tex);              // cpp 409
		device->SetTexture(8, p == 1 && spill ? b.memory[memoryWrite].tex : combine && spill && spillDebug == 1 ? b.spill[0].tex : nullptr); // cpp 412
		CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
		e->EndPass();
	}
	e->End();
	for (int k = 0; k < 16; k++) device->SetTexture(k, nullptr);
	device->EndScene();
	// cpp 429-441: this frame's is next frame's "last frame".
	state.lastView = view; state.lastInvView = invView; state.lastProj = proj; state.lastValid = true; state.frame++;
	state.historyValid = useHistory;
	if (useHistory) state.historyIndex = writeIndex;
	if (spill) { state.memoryIndex = memoryWrite; state.memoryValid = true; } else state.memoryValid = false;
}

// Bloom.cpp's RenderBloomBuffer, 8 passes, additive (PassBlending 0). Reads `image`, leaves the bloom in buffers[0].
struct BloomSettings { float spread, glowBoost, threshold; bool linearLight, antiFlicker; };
static Target bloomBuf[8];
static IDirect3DVertexBuffer9* bloomQuad[8];
static void RunBloom(ID3DXEffect* e, IDirect3DTexture9* image, const BloomSettings& s, bool newFile) {
	const int passes = 8;
	const float w = 1.0f / passes;
	float spread = 1.0f + s.spread, norm = w;
	if (spread != 1.0f) { float total = 0, weight = 1; for (int i = 0; i < passes; i++) { total += weight; weight *= spread; } norm = 1.0f / total; }
	Vec(e, "TESR_BloomData", 0.005f, 0.005f * ((float)W / H), 0.0f, w);
	if (newFile) {
		Vec(e, "TESR_BloomGlow", spread, norm, s.glowBoost, s.threshold);
		Vec(e, "TESR_BloomGlowExtra", s.antiFlicker ? 1.0f : 0.0f, s.linearLight ? 1.0f : 0.0f, 0, 0);
	}
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	CHECK(device->BeginScene());
	auto pass = [&](int technique, int level, bool clear) {
		const Target& t = bloomBuf[level];
		device->SetStreamSource(0, bloomQuad[level], 0, sizeof(QuadVertex));
		Vec(e, "TESR_BloomResolution", (float)t.w, (float)t.h, 1.0f / t.w, 1.0f / t.h);
		e->SetTechnique(e->GetTechnique(technique));
		UINT n; CHECK(e->Begin(&n, 0));
		D3DVIEWPORT9 vp = { 0, 0, (DWORD)t.w, (DWORD)t.h, 0, 1 };
		CHECK(device->SetRenderTarget(0, t.surf)); CHECK(device->SetViewport(&vp));
		if (clear) device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
		CHECK(e->BeginPass(0));
		for (int k = 0; k < 8; k++) device->SetTexture(k, bloomBuf[k].tex);   // as EffectRecord::SetCT binds them
		device->SetTexture(8, image);
		CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
		e->EndPass(); e->End();
	};
	for (int i = 0; i < passes; i++) pass(i, i, true);
	int technique = passes;
	for (int i = passes - 2; i >= 0; i--) pass(technique++, i, false);
	for (int k = 0; k < 16; k++) device->SetTexture(k, nullptr);
	device->EndScene();
}

// The tonemapper's NVR bloom blend (ISHDRBLENDINSHADERCIN): linear image, lerp toward the bloom by Blending, then a filmic curve.
static const char* TonemapFx = R"(
sampler2D Scene : register(s0) = sampler_state { MAGFILTER = POINT; MINFILTER = POINT; };
sampler2D Bloom : register(s1) = sampler_state { MAGFILTER = LINEAR; MINFILTER = LINEAR; ADDRESSU = CLAMP; ADDRESSV = CLAMP; };
float4 Params; // x blending, y strength, z exposure, w: 1 = show the bloom buffer only
float3 lin(float3 c) { c = max(c, 0); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 delin(float3 c) { c = max(c, 0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
float3 ACES(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
struct V { float4 p : POSITION; float2 uv : TEXCOORD0; };
V VS(float4 p : POSITION, float2 uv : TEXCOORD0) { V o; o.p = p; o.uv = uv; return o; }
float4 PS(V i) : COLOR0 {
	float3 c = lin(tex2D(Scene, i.uv).rgb);
	float3 b = lin(tex2D(Bloom, i.uv).rgb);
	c = Params.w > 0.5 ? (Params.w > 1.5 ? tex2D(Bloom, i.uv).rgb : b) * Params.y : lerp(c, b * Params.y, Params.x);
	return float4(delin(ACES(c * Params.z)), 1);
}
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
)";
static ID3DXEffect* tonemap;
static Target ldr;
static void SavePng(IDirect3DTexture9* image, float blending, float strength, float exposure, const std::string& path, bool bloomOnly = false,
	IDirect3DTexture9* other = nullptr, bool linearOther = false) {
	Vec(tonemap, "Params", blending, bloomOnly ? 8.0f : strength, exposure, other ? (linearOther ? 2.0f : 1.0f) : bloomOnly ? 1.0f : 0.0f);
	device->SetStreamSource(0, fullQuad, 0, sizeof(QuadVertex));
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	CHECK(device->BeginScene());
	tonemap->SetTechnique("T");
	UINT n; tonemap->Begin(&n, 0);
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)W, (DWORD)H, 0, 1 };
	device->SetRenderTarget(0, ldr.surf); device->SetViewport(&vp);
	tonemap->BeginPass(0);
	device->SetTexture(0, image); device->SetTexture(1, other ? other : bloomBuf[0].tex);
	device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	tonemap->EndPass(); tonemap->End();
	device->EndScene();
	if (path.empty()) return;   // tonemapped into ldr only (read back by the caller)
	CHECK(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, ldr.surf, NULL, NULL));
	std::printf("wrote %s\n", path.c_str());
}

// The game's HDR blend and tonemap (critic notes 10): ISHDRBLENDINSHADERCIN.pso.hlsl's main() with the game's own
// Includes\\Tonemapping.hlsl, NVR bloom on, the weather's cinematic / tint / fade neutral, the defaults toml's [Shaders.Tonemapping.Main]
// (TonemappingMode 10 = DICE, Exposure 1.5, Gamma 2.2, Linearization 2.2, Saturation 1, Contrast / Brightness / Midpoint / Shoulder /
// WhitePoint 1, HueSafeClip on) and [Shaders.Bloom.Main] (Blending 0.04, Strength 1). Written next to the pictures with its includes.
static const char* GameTonemapFx = R"(
float4 TESR_HDRBloomData; float4 TESR_HDRData; float4 TESR_LotteData; float4 TESR_ToneMapping; float4 ClipOn;
sampler2D Dest : register(s0) = sampler_state { MAGFILTER = POINT; MINFILTER = POINT; ADDRESSU = CLAMP; ADDRESSV = CLAMP; };
sampler2D BloomBuf : register(s1) = sampler_state { MAGFILTER = LINEAR; MINFILTER = LINEAR; ADDRESSU = CLAMP; ADDRESSV = CLAMP; };
#include "Includes/Helpers.hlsl"
#include "Includes/Tonemapping.hlsl"
struct V { float4 p : POSITION; float2 uv : TEXCOORD0; };
V VS(float4 p : POSITION, float2 uv : TEXCOORD0) { V o; o.p = p; o.uv = uv; return o; }
float4 PS(V i) : COLOR0 {
	float3 f = pows(tex2D(Dest, i.uv).rgb, TESR_ToneMapping.w);                     // linearize (Linearization)
	float4 bloom = linearize(tex2D(BloomBuf, i.uv));                               // NVR bloom
	f = lerp(f, bloom.rgb * TESR_HDRBloomData.y, saturate(TESR_HDRBloomData.x));
	f = tonemap(f * TESR_HDRData.y);                                                // exposure + DICE
	[branch] if (ClipOn.x > 0.5f) {                                                 // HueSafeClip, as in the game's shader
		const float peak = max(f.r, max(f.g, f.b));
		[branch] if (peak > 1.0f) {
			const float brightness = saturate(luma(f));
			f = brightness + (f - brightness) * saturate((1.0f - brightness) / max(peak - brightness, 1e-4f));
		}
	}
	f = pows(f, 1 / TESR_HDRData.w);                                                // gamma
	return float4(f, 1);
}
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
)";
static ID3DXEffect* gameTonemap = nullptr;
static float eyeAdaptation = 1.0f; // x the tonemap's Exposure: a stand-in for the game's eye adaptation (not in the preview)
static void LoadGameTonemap(const std::string& fxDir, const std::string& dir) {
	CreateDirectoryA(dir.c_str(), NULL); CreateDirectoryA((dir + "\\Includes").c_str(), NULL);
	for (const char* inc : { "Helpers.hlsl", "Tonemapping.hlsl" })
		CopyFileA((fxDir + "\\..\\Shaders\\Includes\\" + inc).c_str(), (dir + "\\Includes\\" + inc).c_str(), FALSE);
	FILE* out = fopen((dir + "\\GameTonemap.fx").c_str(), "wb"); fwrite(GameTonemapFx, 1, strlen(GameTonemapFx), out); fclose(out);
	ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCreateEffectFromFileA(device, (dir + "\\GameTonemap.fx").c_str(), NULL, NULL, 0, NULL, &gameTonemap, &errors))) {
		std::printf("game tonemap: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?"); std::exit(1);
	}
	Vec(gameTonemap, "TESR_HDRData", 10.0f, 1.5f, 1.0f, 2.2f);
	Vec(gameTonemap, "TESR_LotteData", 1.0f, 1.0f, 1.0f, 1.0f);
	Vec(gameTonemap, "TESR_HDRBloomData", 0.04f, 1.0f, 1.0f, 1.0f);
	Vec(gameTonemap, "TESR_ToneMapping", 1.0f, 0.0f, 1.0f, 2.2f);
	Vec(gameTonemap, "ClipOn", 1.0f, 0, 0, 0);
}
// image (display-encoded HDR, as the game's) + the bloom chain's result in bloomBuf[0] -> ldr, through the game's tonemap.
static void GameTonemapTo(IDirect3DTexture9* image, IDirect3DTexture9* bloom, const std::string& path = "");

static void GameTonemapTo(IDirect3DTexture9* image, IDirect3DTexture9* bloom, const std::string& path) {
	device->SetStreamSource(0, fullQuad, 0, sizeof(QuadVertex));
	device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
	CHECK(device->BeginScene());
	Vec(gameTonemap, "TESR_HDRData", 10.0f, 1.5f * eyeAdaptation, 1.0f, 2.2f);
	gameTonemap->SetTechnique("T");
	UINT n; gameTonemap->Begin(&n, 0);
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)W, (DWORD)H, 0, 1 };
	device->SetRenderTarget(0, ldr.surf); device->SetViewport(&vp);
	gameTonemap->BeginPass(0);
	device->SetTexture(0, image); device->SetTexture(1, bloom);
	device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	gameTonemap->EndPass(); gameTonemap->End();
	device->EndScene();
	if (path.empty()) return;
	CHECK(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, ldr.surf, NULL, NULL));
	std::printf("wrote %s\n", path.c_str());
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

// ---- measurements ----
// The light the bounce/spill added to each full-resolution pixel (linear luminance of b.out minus the scene's).
static std::vector<float> AddedLight(const BounceBuffers& b) {
	auto out = ReadF16(b.out);
	std::vector<float> added(W * H);
	for (int i = 0; i < W * H; i++) {
		float o[3], s[3];
		for (int k = 0; k < 3; k++) { o[k] = ToLinear(out[i * 4 + k]); s[k] = ToLinear(scene.colour[i * 4 + k]); }
		added[i] = Luma(o) - Luma(s);
	}
	return added;
}
// The spill alone (the spill gather's buffer, low resolution) at each full-resolution pixel's low-resolution texel.
static std::vector<float> SpillAt(const BounceBuffers& b) {
	auto m = ReadF16(b.memory[state.memoryIndex]);
	const int lw = b.prep.w, lh = b.prep.h;
	std::vector<float> v(W * H);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		const int lx = min(lw - 1, x * lw / W), ly = min(lh - 1, y * lh / H);
		v[y * W + x] = Luma(&m[(ly * lw + lx) * 4]);
	}
	return v;
}
// Mean of `v` over the pixels `pick` accepts (and how many).
template <class P> static double MeanWhere(const std::vector<float>& v, P pick, int* count = nullptr) {
	double s = 0; int n = 0;
	for (int i = 0; i < W * H; i++) if (pick(i)) { s += v[i]; n++; }
	if (count) *count = n;
	return n ? s / n : 0.0;
}
static float DistTo(int i, float x, float y, float z) {
	const float dx = scene.world[i * 3] - x, dy = scene.world[i * 3 + 1] - y, dz = scene.world[i * 3 + 2] - z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

int main(int argc, char** argv) {
	if (argc < 5) { std::puts("usage: glow_preview <Effects folder> <pre-spill Effects folder> <round-5 Effects folder> <output folder> [width height [sceneScale]]"); return 2; }
	const std::string fxDir = argv[1], oldDir = argv[2], r5Dir = argv[3], outDir = argv[4];
	if (argc > 6) { W = atoi(argv[5]); H = atoi(argv[6]); }
	if (argc > 7) sceneScale = (float)atof(argv[7]);

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "GlowPreview"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("GlowPreview", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window;
	CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	SetCamera(6.0f);
	const std::string noisePath = fxDir + "\\..\\..\\..\\..\\resource\\Textures\\Effects\\bluenoise256.dds";
	CHECK(D3DXCreateTextureFromFileExA(device, noisePath.c_str(), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0, D3DFMT_UNKNOWN,
		D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, NULL, NULL, &noiseTex));

	fullQuad = MakeQuad(W, H);
	{ int w = W, h = H; for (int i = 0; i < 8; i++) { w /= 2; h /= 2; bloomBuf[i] = MakeTarget(w, h, D3DFMT_A16B16G16R16F); bloomQuad[i] = MakeQuad(w, h); } }
	ldr = MakeTarget(W, H, D3DFMT_A8R8G8B8);
	ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCreateEffect(device, TonemapFx, (UINT)strlen(TonemapFx), NULL, NULL, 0, NULL, &tonemap, &errors))) {
		std::printf("tonemap: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?"); return 1;
	}
	ID3DXEffect* bounceNew = LoadEffect(fxDir + "\\BounceLight.fx.hlsl");
	ID3DXEffect* bounceOld = LoadEffect(oldDir + "\\BounceLight.fx.hlsl");
	ID3DXEffect* bounceR5 = LoadEffect(r5Dir + "\\BounceLight.fx.hlsl");
	ID3DXEffect* bloomNew = LoadEffect(fxDir + "\\Bloom.fx.hlsl");
	ID3DXEffect* bloomOld = LoadEffect(oldDir + "\\Bloom.fx.hlsl");
	BounceBuffers bb = MakeBounceBuffers();
	BigSignFlux();
	LoadGameTonemap(fxDir, outDir + "\\fx_gametonemap");

	int failures = 0;
	const char* techniqueNames[4] = { "first technique", "BounceLightPlus", "BounceLightTemporal", "BounceLightTemporalHalf" };
	auto part = [](char n) { const char* p = getenv("GLOW_PARTS"); return !p || strchr(p, n); };
	// M1: the game's light test vs a white surface in the scene's light.
	for (int day = 0; day < 2; day++) {
		dayLight = day != 0; sceneId = 0;
		float a[3], d[3]; OutdoorLight(a, d);
		const float white = 0.2126f * (a[0] + d[0]) + 0.7152f * (a[1] + d[1]) + 0.0722f * (a[2] + d[2]);
		std::printf("M1 %s: the game's light test %.3f, a white surface facing the %s in the scene %.3f\n", day ? "dusk" : "night", SceneLight(), day ? "sun" : "moon", white);
	}
	dayLight = false; SetCamera(6.0f);
	// 1. Off = as before, bit for bit, over 4 consecutive frames with history (the camera turning 0.5 degrees a frame).
	//    (a) NeonSpill 0: the new file vs the pre-spill file, all four techniques, MultiBounce 1 and OffScreenLight 1 where they apply.
	//    (b) NeonSpill 2, the round-6 keys 0: the new file vs round 5's file (Plus, Temporal, TemporalHalf).
	if (part('1')){
		const BloomSettings off = { 0, 0, 6.0f, false, false };
		RunBloom(bloomOld, sceneTex, off, false); auto a = ReadBack(bloomBuf[0], D3DFMT_A16B16G16R16F);
		RunBloom(bloomNew, sceneTex, off, true); auto b = ReadBack(bloomBuf[0], D3DFMT_A16B16G16R16F);
		bool same = a == b; failures += !same;
		std::printf("%s bloom, new settings off vs old file: %s\n", same ? "PASS" : "FAIL", same ? "identical" : "DIFFERENT");
		auto sequence = [&](ID3DXEffect* e, const BounceSettings& s, bool newFile) {
			std::vector<std::vector<unsigned char>> frames;
			ClearBounceBuffers(bb);
			for (int f = 0; f < 4; f++) {
				SetCamera(6.0f - 0.5f * f);
				RunBounce(e, bb, s, newFile, f > 0);
				frames.push_back(ReadBack(bb.out, D3DFMT_A16B16G16R16F));
			}
			return frames;
		};
		for (int t = 0; t < 4; t++) {
			BounceSettings s = { 1.5f, 0.0f, 1.0f, 1.5f, t }; s.offScreen = t ? 1.0f : 0.0f; s.multiBounce = t ? 1.0f : 0.0f;
			auto c = sequence(bounceOld, s, false), d = sequence(bounceNew, s, true);
			same = c == d; failures += !same;
			std::printf("%s (a) NeonSpill 0, %s, 4 frames: new file vs pre-spill file %s\n", same ? "PASS" : "FAIL", techniqueNames[t], same ? "identical" : "DIFFERENT");
			const bool r5same = sequence(bounceR5, s, true) == c;   // for reference: round 5's own file (not counted)
			std::printf("     round 5's file vs the pre-spill file, %s: %s\n", techniqueNames[t], r5same ? "identical" : "DIFFERENT");
		}
		{   // B-15: the spill on for 2 frames, then everything off (the first technique) for 4: the new file vs the pre-spill file.
			BounceSettings on = UserLike(2.0f), off = { 1.5f, 0.0f, 1.0f, 1.5f, 0 };
			spillShape[0] = 0.5f; spillShape[1] = 0.5f; spillShape[2] = 16.0f;
			ClearBounceBuffers(bb);
			for (int f = 0; f < 2; f++) { SetCamera(6.0f); RunBounce(bounceNew, bb, on, true, f > 0); }
			std::vector<std::vector<unsigned char>> afterSpill;
			for (int f = 0; f < 4; f++) { SetCamera(6.0f - 0.5f * f); RunBounce(bounceNew, bb, off, true, true); afterSpill.push_back(ReadBack(bb.out, D3DFMT_A16B16G16R16F)); }
			const auto reference = sequence(bounceOld, off, false);
			same = afterSpill == reference; failures += !same;
			// The control: round 5's file driven by round 5's C++ (no B-15) the same way must differ, or this check proves nothing.
			setSpillOff = false; ClearBounceBuffers(bb);
			for (int f = 0; f < 2; f++) { SetCamera(6.0f); RunBounce(bounceR5, bb, on, true, f > 0); }
			std::vector<std::vector<unsigned char>> control;
			for (int f = 0; f < 4; f++) { SetCamera(6.0f - 0.5f * f); RunBounce(bounceR5, bb, off, true, true); control.push_back(ReadBack(bb.out, D3DFMT_A16B16G16R16F)); }
			setSpillOff = true;
			std::printf("     control: round 5 (file and C++) the same way vs the pre-spill file: %s (expected DIFFERENT: the stale value)\n", control == reference ? "identical" : "DIFFERENT");
			std::printf("%s B-15 NeonSpill 2 for 2 frames, then everything off (first technique), 4 frames: new file vs pre-spill file %s\n",
				same ? "PASS" : "FAIL", same ? "identical" : "DIFFERENT");
		}
		{   // The five keys at the defaults package's values (SeparateDepth 1, HardEdges 1, CapSceneLight 1, MaskSpillOnly 1, HoldAfterTurn 1 s) with NeonSpill 0: as before, bit for bit.
			for (int t = 0; t < 4; t++) {
				BounceSettings s = { 1.5f, 0.0f, 1.0f, 1.5f, t }; s.offScreen = t ? 1.0f : 0.0f; s.multiBounce = t ? 1.0f : 0.0f;
				BounceSettings k = s; k.pyramidFix = k.hardEdges = k.capScene = k.maskSpill = 1.0f; k.holdAfter = 1.0f;
				same = sequence(bounceNew, k, true) == sequence(bounceOld, s, false); failures += !same;
				std::printf("%s (a, keys) NeonSpill 0 with all five keys on, %s, 4 frames: new file vs pre-spill file %s\n",
					same ? "PASS" : "FAIL", techniqueNames[t], same ? "identical" : "DIFFERENT");
			}
			// B-15 with all five keys on: NeonSpill 2 for 2 frames, then everything off for 4 (the camera turns, so HoldAfterTurn's timer runs).
			BounceSettings on = UserLike(2.0f), off = { 1.5f, 0.0f, 1.0f, 1.5f, 0 };
			on.pyramidFix = on.hardEdges = on.capScene = on.maskSpill = on.holdAfter = 1.0f; off.pyramidFix = off.hardEdges = off.capScene = off.maskSpill = off.holdAfter = 1.0f;
			spillShape[0] = 0.5f; spillShape[1] = 0.5f; spillShape[2] = 16.0f;
			ClearBounceBuffers(bb);
			for (int f = 0; f < 2; f++) { SetCamera(6.0f); RunBounce(bounceNew, bb, on, true, f > 0); }
			std::vector<std::vector<unsigned char>> afterSpill;
			for (int f = 0; f < 4; f++) { SetCamera(6.0f - 0.5f * f); RunBounce(bounceNew, bb, off, true, true); afterSpill.push_back(ReadBack(bb.out, D3DFMT_A16B16G16R16F)); }
			BounceSettings plain = off; plain.pyramidFix = plain.hardEdges = plain.capScene = plain.maskSpill = plain.holdAfter = 0.0f;
			same = afterSpill == sequence(bounceOld, plain, false); failures += !same;
			std::printf("%s B-15 with all five keys on: NeonSpill 2 for 2 frames, then everything off, 4 frames: new file vs pre-spill file %s\n",
				same ? "PASS" : "FAIL", same ? "identical" : "DIFFERENT");
		}
		spillShape[0] = 0.5f; spillShape[1] = 1.0f; spillShape[2] = 16.0f;   // round 5's defaults
		for (int day = 0; day < 2; day++) for (int prof = 0; prof < 2; prof++) for (int t = 1; t < 4; t++) {
			if (day && t != 2) continue;   // dusk/day: the user's path only
			dayLight = day != 0;
			BounceSettings s = prof ? Defaults() : UserLike(2.0f); s.technique = t;
			spillShape[1] = prof ? 1.0f : 0.5f;   // NeonSpillFar: defaults 1, yours 0.5
			auto c = sequence(bounceR5, s, true), d = sequence(bounceNew, s, true);
			same = c == d; failures += !same;
			std::printf("%s (b) %s, %s, new keys 0, %s, 4 frames: new file vs round-5 file %s\n", same ? "PASS" : "FAIL", day ? "dusk" : "night",
				prof ? "defaults (NeonSpill 1, Reach 1, LightLevel 1.5, Far 1)" : "yours (NeonSpill 2, Reach 0.25, LightLevel 3, Far 0.5)", techniqueNames[t], same ? "identical" : "DIFFERENT");
		}
		dayLight = false;
		spillShape[0] = 1.0f; spillShape[1] = 1.0f; spillShape[2] = 0.0f;
		SetCamera(6.0f);
	}

	// (M3 runs after the identity checks: it runs the new file with the spill on, and technique 0 never sets NVR_BounceSpill, so a
	// stale value from a spill-on run would switch on Combine's and Blur's spill-only parts there; see the report, round 5 does the same.)
	{   // M3: the sampler states, each technique with the spill on, and the separate gather
		checkSamplers = true;
		for (int t = 0; t < 4; t++) for (int hard = 0; hard < 2; hard++) { BounceSettings s = UserLike(2.0f); s.technique = t; s.hardEdges = (float)hard; RunBounce(bounceNew, bb, s, true); RunBounce(bounceNew, bb, s, true, true); }
		BounceSettings s = UserLike(2.0f); s.pyramidFix = 1.0f; RunBounce(bounceNew, bb, s, true);
		checkSamplers = false;
		std::printf("%s M3 sampler states: %d checks, %d wrong\n", samplerFailures ? "FAIL" : "PASS", samplerChecks, samplerFailures);
		failures += samplerFailures > 0;
	}
	// 2. Pictures (single frames, BounceLightPlus as before).
	const float exposure = 0.45f / sceneScale;   // a white surface in the game's night light is about 1.9
	const BloomSettings bloomOff = { 0, 0, 6.0f, false, false };
	const BloomSettings bloomNew2 = { 0.6f, 4.0f, 6.0f, true, true };
	struct Shot { const char* name; BounceSettings bounce; bool newBloom; float blending; };
	const Shot shots[] = {
		{ "1_now", { 0, 0, 1, 1.5f, 0 }, false, 0.04f },
		{ "2_spill", { 0, 2.0f, 1, 1.5f, 1 }, false, 0.04f },
		{ "3_spill_bounce", { 1.5f, 2.0f, 1, 1.5f, 1 }, false, 0.04f },
		{ "5_spill_new_bloom", { 1.5f, 2.0f, 1, 1.5f, 1 }, true, 0.15f },
		{ "7_spill_strong", { 0, 5.0f, 1, 1.5f, 1 }, false, 0.04f },
	};
	if (part('2')) for (int pass = 0; pass < 3; pass++)   // round 4's shape (near 1, far 1, no cap), round 5's defaults, round 5 + NeonSpillSeparateDepth
	for (const Shot& shot : shots) {
		if (pass > 0 && shot.bounce.spill <= 0) continue;
		spillShape[0] = pass ? 0.5f : 1.0f; spillShape[1] = 1.0f; spillShape[2] = pass ? 16.0f : 0.0f;
		BounceSettings s = shot.bounce; s.pyramidFix = pass == 2 ? 1.0f : 0.0f;
		IDirect3DTexture9* image = sceneTex;
		if (s.technique) { RunBounce(bounceNew, bb, s, true); image = bb.out.tex; }
		RunBloom(bloomNew, image, shot.newBloom ? bloomNew2 : bloomOff, true);
		SavePng(image, shot.blending, 1.0f, exposure, outDir + "\\glow_" + shot.name + (s.technique == 0 ? "" : pass == 1 ? "_r5" : pass == 2 ? "_r5_separatedepth" : "_r4") + ".png");
	}
	if (part('2')) {   // debug: the spill pyramid and the gather (linear values x 20)
		spillShape[0] = 0.5f; spillShape[1] = 1.0f; spillShape[2] = 16.0f;
		BounceSettings s = { 0, 2.0f, 1, 1.5f, 1 };
		RunBounce(bounceNew, bb, s, true);
		for (int i = 0; i < 7; i += 3) SavePng(sceneTex, 0, 20.0f, 1.0f, outDir + "\\debug_spill_level" + std::to_string(i) + ".png", false, bb.spill[i].tex, true);
		SavePng(sceneTex, 0, 20.0f, 1.0f, outDir + "\\debug_bounce_result.png", false, bb.low[0].tex, true);
		auto lvl = ReadF16(bb.spill[0]);
		float mx[4] = { 0, 0, 0, 0 }; int nothing = 0;
		for (size_t k = 0; k < lvl.size() / 4; k++) { for (int c = 0; c < 4; c++) mx[c] = max(mx[c], lvl[k * 4 + c]); nothing += lvl[k * 4 + 3] >= 0.999f; }
		std::printf("spill level 0: max rgb %.3f %.3f %.3f, %d of %d texels empty (depth = SpillNothing 1)\n", mx[0], mx[1], mx[2], nothing, (int)(lvl.size() / 4));
		for (int d = 1; d <= 2; d++) {   // the NeonSpillDebug views
			spillDebug = d;
			RunBounce(bounceNew, bb, BounceSettings{ 1.5f, 2.0f, 1, 1.5f, 1 }, true);
			RunBloom(bloomNew, bb.out.tex, bloomOff, true);
			SavePng(bb.out.tex, 0.0f, 1.0f, exposure, outDir + "\\debug_view_" + std::to_string(d) + ".png");
		}
		spillDebug = 0;
	}

	// 3. Memory check: frame 0 normal, frame 1 with nothing counting as a light (light level 100000).
	if (part('3')){
		SetCamera(6.0f);
		auto total = [&](const Target& t) { auto m = ReadF16(t); double r = 0; for (size_t k = 0; k < m.size() / 4; k++) r += m[k * 4]; return r; };
		BounceSettings a = { 1.5f, 3.0f, 1, 1.5f, 1 }, dark = { 1.5f, 3.0f, 1, 100000.0f, 1 };
		RunBounce(bounceNew, bb, a, true, false);
		const double r0 = total(bb.memory[state.memoryIndex]);
		RunBounce(bounceNew, bb, dark, true, true);
		const double r1 = total(bb.memory[state.memoryIndex]);
		std::printf("memory check, still camera: kept %.2f (expect 0.85)\n", r1 / max(r0, 1e-6));
		for (int axis = 0; axis < 2; axis++) {
			SetCamera(6.0f, -6.0f);
			RunBounce(bounceNew, bb, a, true, false);
			const double m0 = total(bb.memory[state.memoryIndex]);
			if (axis == 0) SetCamera(6.0f, -4.5f); else SetCamera(4.5f, -6.0f);
			RunBounce(bounceNew, bb, dark, true, true);
			const double m1 = total(bb.memory[state.memoryIndex]);
			std::printf("memory check, camera %s 1.5 degrees: kept %.2f (expect about 0.98: while the view moves the hold term keeps exp(-dt/1 s))\n",
				axis == 0 ? "tilted down" : "turned left", m1 / max(m0, 1e-6));
		}
	}

	// Every acceptance check from here on runs at two settings (critic review 6): the defaults toml's and the user's current ones.
	struct Profile { const char* name; float strength, spill, reach, lightLevel, nearW, farW; };
	const Profile profiles[2] = {
		{ "DEFAULTS (NeonSpill 1, Reach 1, LightLevel 1.5, Near 0.5, Far 1, Strength 1.5)", 1.5f, 1.0f, 1.0f, 1.5f, 0.5f, 1.0f },
		{ "YOURS (NeonSpill 2, Reach 0.25, LightLevel 3, Near 0.5, Far 0.5, Strength 0.5)", 0.5f, 2.0f, 0.25f, 3.0f, 0.5f, 0.5f } };
	auto shapeFor = [](const Profile& p) { spillShape[0] = p.nearW; spillShape[1] = p.farW; spillShape[2] = 16.0f; };
	// The game's path (Temporal, MultiBounce 1, OffScreenLight 1) at a profile.
	auto pathFor = [](const Profile& p) { BounceSettings s = { p.strength, p.spill, p.reach, p.lightLevel, 2 }; s.offScreen = 1.0f; s.multiBounce = 1.0f; return s; };
	// A still frame of the spill alone (BounceLightPlus, Strength 0) at a profile.
	auto spillOnly = [](const Profile& p) { BounceSettings s = { 0.0f, p.spill, p.reach, p.lightLevel, 1 }; return s; };
	auto tag = [](bool pass) { return pass ? "PASS" : "FAIL"; };

	// 4. B-2 coplanar test (issues 3/4): a flat wall with a pink sign painted on it (box 1, -y face, sign x 140-260, z 160-240) and a
	// wall at right angles to it (box 2, -x face at x 300), seen obliquely and from straight on. The spill alone (the spill gather's
	// buffer, one still frame) received on each wall in rings by distance from the sign's edge; all levels, levels 0-2 only, levels
	// 3-6 only. Lambert: the sign's own wall should get about nothing. Pass: every ring <= 10% of round 5's.
	auto coplanar = [&](const Profile& prof) {
		shapeFor(prof);
		const float rings[] = { 0, 40, 100, 200, 400, 700 };
		auto edge = [&](int i) { // distance from the sign's rectangle (x 140-260, z 160-240 on y 1000)
			const float dx = max(0.0f, max(140.0f - scene.world[i * 3], scene.world[i * 3] - 260.0f));
			const float dz = max(0.0f, max(160.0f - scene.world[i * 3 + 2], scene.world[i * 3 + 2] - 240.0f));
			const float dy = scene.world[i * 3 + 1] - 1000.0f;
			return sqrtf(dx * dx + dy * dy + dz * dz); };
		for (int view = 0; view < 2; view++) {
			if (view == 0) LookAt(2, D3DXVECTOR3(-500, 350, 170), D3DXVECTOR3(150, 1000, 200));
			else LookAt(2, D3DXVECTOR3(60, 250, 180), D3DXVECTOR3(60, 1000, 180));
			double ringR5[5] = {};
			bool pass = true;
			for (int fix = 0; fix < 2; fix++) {
				std::printf("coplanar, %s, %s, %s view, spill received (luma x 1000) by distance from the sign's edge:\n", prof.name,
					fix ? "NeonSpillSeparateDepth" : "round 5", view ? "straight-on" : "oblique");
				for (int levels = 0; levels < 3; levels++) {
					pyramidKeep = levels == 0 ? 0x7F : levels == 1 ? 0x07 : 0x78;
					BounceSettings s = spillOnly(prof); s.pyramidFix = (float)fix;
					RunBounce(bounceNew, bb, s, true);
					auto spillMap = SpillAt(bb);
					std::printf("  levels %s:", levels == 0 ? "0-6" : levels == 1 ? "0-2" : "3-6");
					double sumSign = 0, sumSide = 0;
					for (int r = 0; r + 1 < 6; r++) {
						int nSign = 0, nSide = 0;
						const double onSign = MeanWhere(spillMap, [&](int i) { float d = edge(i);
							return scene.box[i] == 1 && scene.world[i * 3 + 1] < 1000.5f && scene.emit[i] == 0 && d >= rings[r] && d < rings[r + 1]; }, &nSign);
						const double onSide = MeanWhere(spillMap, [&](int i) { float d = edge(i);
							return scene.box[i] == 2 && scene.world[i * 3] < 300.5f && d >= rings[r] && d < rings[r + 1]; }, &nSide);
						std::printf("  %g-%g: sign wall %.3f, right-angle wall %.3f", rings[r], rings[r + 1], onSign * 1000, onSide * 1000);
						if (nSign && nSide) { sumSign += onSign; sumSide += onSide; }
						if (levels == 0 && fix == 0) ringR5[r] = onSign;
						if (levels == 0 && fix == 1 && onSign > 0.1 * ringR5[r] + 1e-7) pass = false;
					}
					std::printf("  | sign wall / right-angle wall = %.1f%%\n", sumSide > 0 ? 100.0 * sumSign / sumSide : -1.0);
					if (levels == 0 && &prof == &profiles[0]) {
						spillDebug = 2; RunBounce(bounceNew, bb, s, true); spillDebug = 0;
						SavePng(bb.out.tex, 0.0f, 1.0f, exposure, outDir + "\\coplanar_defaults_" + (fix ? "separatedepth" : "r5") + (view ? "_straight" : "_oblique") + ".png");
					}
				}
			}
			std::printf("  [B-2 coplanar, %s, %s view: every ring <= 10%% of round 5's: %s]\n", prof.name, view ? "straight-on" : "oblique", tag(pass));
		}
		pyramidKeep = 0x7F;
	};
	if (part('4')) for (const Profile& prof : profiles) coplanar(prof);

	// 5. Street numbers (pass criteria for B-2): mean light added to the ground (road + pavement) with the spill alone (Strength 0) and
	// with the profile's bounce strength, on the pink sign's panel between its tubes (issue 4) and on its wall just outside it;
	// round 5 vs NeonSpillSeparateDepth, single still frames (BounceLightPlus). Pass: the ground within 15% of round 5.
	if (part('5')) for (const Profile& prof : profiles) {
		SetCamera(6.0f);
		shapeFor(prof);
		double v[2][4];
		auto ground = [&](int i) { return scene.box[i] == 0 || scene.box[i] == 1; };
		auto panel = [&](int i) { return scene.box[i] == 3 && scene.emit[i] == 0 && scene.world[i * 3] < 420.5f &&
			scene.world[i * 3 + 1] > 480 && scene.world[i * 3 + 1] < 820 && scene.world[i * 3 + 2] > 180 && scene.world[i * 3 + 2] < 620; };
		auto wallOut = [&](int i) { float y = scene.world[i * 3 + 1], z = scene.world[i * 3 + 2];
			bool outside = y < 430 || y > 870 || z < 130 || z > 670; bool inBand = y > 330 && y < 970 && z > 30 && z < 770;
			return scene.box[i] == 3 && scene.world[i * 3] < 420.5f && outside && inBand; };
		for (int fix = 0; fix < 2; fix++) {
			BounceSettings a = spillOnly(prof), b = spillOnly(prof); b.strength = prof.strength;
			a.pyramidFix = b.pyramidFix = (float)fix;
			RunBounce(bounceNew, bb, a, true); auto addedSpill = AddedLight(bb);
			RunBounce(bounceNew, bb, b, true); auto addedAll = AddedLight(bb);
			v[fix][0] = MeanWhere(addedSpill, ground); v[fix][1] = MeanWhere(addedAll, ground);
			v[fix][2] = MeanWhere(addedAll, panel); v[fix][3] = MeanWhere(addedAll, wallOut);
		}
		// A reference for the ground: the estimator's own model (each light pixel's light x its area / (pi d^2 + area) x the cosine at the
		// receiver x the Near-Far weight of its screen distance) summed on the CPU over every light pixel the reads can reach (the farthest read plus one coarsest texel), no
		// occlusion. Ground pixels every 4th row and column; light pixels every 2nd (area x 4). Same units as the spill buffer.
		{
			const float t = prof.lightLevel * SceneLight(), first = prof.reach * 1.5f * W / 320.0f, reachPx = 64.0f * first * 1.25f + 64.0f * W / 320.0f; // the farthest read (jitter 1.25) plus one level-6 texel (its bilinear footprint)
			const float pixelWorld = 2.0f / (W * proj._11);
			struct Emitter { float x, y, z, px, py, light, area; };
			std::vector<Emitter> em;
			for (int py = 0; py < H; py += 2) for (int px = 0; px < W; px += 2) {
				const int i = py * W + px; if (scene.box[i] < 0) continue;
				float c[3]; for (int k = 0; k < 3; k++) c[k] = ToLinear(scene.colour[i * 4 + k]);
				const float peak = max(c[0], max(c[1], c[2])), sat = 1.0f - min(c[0], min(c[1], c[2])) / max(peak, 1e-4f);
				const float b = Luma(c) + (peak - Luma(c)) * sat;
				float amount = min(1.0f, max(0.0f, (b - t) / t)); if (spillShape[2] > 0) amount *= min(1.0f, spillShape[2] * t / max(b, 1e-4f));
				if (amount <= 0) continue;
				const float z = scene.depth[i] * FarZ, area = 4.0f * pixelWorld * pixelWorld * z * z;
				em.push_back({ scene.world[i * 3], scene.world[i * 3 + 1], scene.world[i * 3 + 2], (float)px, (float)py, Luma(c) * amount, area });
			}
			double sum = 0; int n = 0; std::vector<float> spillR5, spillFix;
			for (int fix = 0; fix < 2; fix++) { BounceSettings a = spillOnly(prof); a.pyramidFix = (float)fix; RunBounce(bounceNew, bb, a, true); (fix ? spillFix : spillR5) = SpillAt(bb); }
			double r5 = 0, sep = 0;
			for (int py = 0; py < H; py += 4) for (int px = 0; px < W; px += 4) {
				const int i = py * W + px; if (!ground(i)) continue;
				double e = 0;
				for (const Emitter& m : em) {
					const float sx = m.px - px, sy = m.py - py, dpx = sqrtf(sx * sx + sy * sy);
					if (dpx > reachPx || dpx < 1.0f) continue;
					const float dx = m.x - scene.world[i * 3], dy = m.y - scene.world[i * 3 + 1], dz = m.z - scene.world[i * 3 + 2];
					const float d2 = dx * dx + dy * dy + dz * dz + 1.0f, cosr = max(0.0f, dz / sqrtf(d2));
					const float share = min(1.0f, max(0.0f, log2f(max(dpx / first, 1.0f)) / 6.0f));
					e += m.light * m.area * cosr / (3.14159265f * d2 + m.area) * (spillShape[0] + (spillShape[1] - spillShape[0]) * share);
				}
				sum += e; r5 += spillR5[i]; sep += spillFix[i]; n++;
			}
			std::printf("street, %s, ground: the estimator's own model on the CPU (no occlusion) %.5f; spill buffer round 5 %.5f (x%.2f), NeonSpillSeparateDepth %.5f (x%.2f)\n",
				prof.name, sum / max(n, 1), r5 / max(n, 1), r5 / max(sum, 1e-12), sep / max(n, 1), sep / max(sum, 1e-12));
		}
		const char* names[4] = { "ground, spill alone", "ground, spill + bounce", "pink panel between tubes", "pink sign's wall 10-100 units outside it" };
		for (int k = 0; k < 4; k++)
			std::printf("street, %s, %s: added light round 5 %.5f, NeonSpillSeparateDepth %.5f (%+.1f%%)%s\n", prof.name, names[k], v[0][k], v[1][k],
				100.0 * (v[1][k] / max(v[0][k], 1e-9) - 1.0), k < 2 ? (fabs(v[1][k] / max(v[0][k], 1e-9) - 1.0) <= 0.15 ? "  [within 15%: PASS]" : "  [outside 15%: FAIL]") : "");
	}

	// 13. B-2 sign colour (critic review 6): one sign on a flat wall (the coplanar scene, oblique and straight-on views), the game's
	// night light (M1: white surface 1.897) and game-bright neon (x 6: red 9 linear, so it counts as a light at LightLevel 1.5, partly
	// at 3 and not at 6 or 12), the game's path, 8 still frames, then the preview's bloom and
	// tonemap (exposure 0.3, so the sign itself is not clipped white). Read back the final picture: the sign's own pixels and its
	// ring (its wall within 40 units of its edge: its backing), mean brightness (display luminance) and colour kept (mean saturation),
	// each vs the spill-off picture. LightLevel 1.5 / 3 / 6 / 12, defaults (NeonSpill 1) and yours (NeonSpill 2), round 5's
	// light-pixel mask on and off (an edited copy without it), NeonSpillSeparateDepth off and on. Pass, key on: sign >= 95% colour
	// and <= +5% brightness, ring <= 1.15x, at all four levels.
	if (part('d')) {
		ID3DXEffect* bounceNoMask = EditedCopy(fxDir, outDir + "\\fx_nomask", { {
			"[branch] if (NVR_BounceSpill.x > 0.0f && NVR_SpillOptions.z <= 0.0f) indirect *= 1.0f - SpillAmount(lin, max(NVR_SpillMemory.w, 0.002f));", "" } });
		auto edge = [&](int i) {
			const float dx = max(0.0f, max(140.0f - scene.world[i * 3], scene.world[i * 3] - 260.0f));
			const float dz = max(0.0f, max(160.0f - scene.world[i * 3 + 2], scene.world[i * 3 + 2] - 240.0f));
			return sqrtf(dx * dx + dz * dz); };
		auto signPx = [&](int i) { return scene.box[i] == 1 && scene.emit[i] > 0; };
		auto ringPx = [&](int i) { return scene.box[i] == 1 && scene.emit[i] == 0 && scene.world[i * 3 + 1] < 1000.5f && edge(i) < 40.0f; };
		auto finalPicture = [&](ID3DXEffect* e, const BounceSettings& s) {
			ClearBounceBuffers(bb);
			for (int f = 0; f < 8; f++) RunBounce(e, bb, s, true, f > 0);
			RunBloom(bloomNew, bb.out.tex, bloomOff, true);
			GameTonemapTo(bb.out.tex, bloomBuf[0].tex);   // the game's HDR blend + DICE tonemap + HueSafeClip
			return ReadBack(ldr, D3DFMT_A8R8G8B8);
		};
		auto measureArea = [&](const std::vector<unsigned char>& img, bool sign, double* luma, double* sat) {
			double l = 0, c = 0; int n = 0;
			for (int i = 0; i < W * H; i++) {
				if (!(sign ? signPx(i) : ringPx(i))) continue;
				const float b = img[i * 4] / 255.0f, g = img[i * 4 + 1] / 255.0f, r = img[i * 4 + 2] / 255.0f;
				const float mx = max(r, max(g, b)), mn = min(r, min(g, b));
				l += 0.2126 * r + 0.7152 * g + 0.0722 * b; c += mx > 1e-4f ? (mx - mn) / mx : 0.0f; n++;
			}
			*luma = l / max(n, 1); *sat = c / max(n, 1);
		};
		const float levels[4] = { 1.5f, 3.0f, 6.0f, 12.0f };
		// The sign x 4 (red 6 linear: a full light at LightLevel 1.5, partly at 3, not at 6 or 12) and the exposure x 0.1 for eye
		// adaptation to the bright Strip, so the sign keeps its colour in the spill-off picture (without it DICE takes it to white).
		neonGain = 4.0f; eyeAdaptation = 0.1f;
		for (int view = 0; view < 2; view++) {
			if (view == 0) LookAt(2, D3DXVECTOR3(-500, 350, 170), D3DXVECTOR3(150, 1000, 200));
			else LookAt(2, D3DXVECTOR3(60, 250, 180), D3DXVECTOR3(60, 1000, 180));
			for (const Profile& prof : profiles) {
				shapeFor(prof);
				BounceSettings off = pathFor(prof); off.spill = 0.0f;
				auto ref = finalPicture(bounceNew, off);
				double refSignL, refSignC, refRingL, refRingC;
				measureArea(ref, true, &refSignL, &refSignC); measureArea(ref, false, &refRingL, &refRingC);
				std::printf("B-2 sign colour, %s view, %s (the game's night light, neon x 4, the game's DICE tonemap, eye adaptation stand-in x 0.1): spill off: sign brightness %.3f saturation %.3f, ring %.3f\n",
					view ? "straight-on" : "oblique", prof.name, refSignL, refSignC, refRingL);
				for (int fix = 0; fix < 2; fix++) for (int mask = 1; mask >= 0; mask--) {
					bool pass = true;
					std::printf("  %-24s mask %-3s:", fix ? "NeonSpillSeparateDepth" : "round 5", mask ? "on" : "off");
					for (float level : levels) {
						BounceSettings s = pathFor(prof); s.lightLevel = level; s.pyramidFix = (float)fix;
						auto img = finalPicture(mask ? bounceNew : bounceNoMask, s);
						double signL, signC, ringL, ringC;
						measureArea(img, true, &signL, &signC); measureArea(img, false, &ringL, &ringC);
						const double kept = signC / max(refSignC, 1e-6), bright = signL / max(refSignL, 1e-6), ring = ringL / max(refRingL, 1e-6);
						// critic notes 10 (c): the sign keeps >= 95% of its colour, the ring >= 100% of its saturation; (a): the ring <= 1.15x brightness.
					pass = pass && kept >= 0.95 && ringC / max(refRingC, 1e-6) >= 0.995 && ring <= 1.15;   // (99.5%: rounding where nothing is added)
						std::printf("  LL %g: sign colour %.0f%% bright x%.3f, ring x%.3f colour %.0f%%;", level, 100.0 * kept, bright, ring, 100.0 * ringC / max(refRingC, 1e-6));
						if (view == 0 && &prof == &profiles[1] && level == 3.0f && mask == 1)
							GameTonemapTo(bb.out.tex, bloomBuf[0].tex, outDir + "\\signcolour_yours_ll3_" + (fix ? "separatedepth" : "r5") + ".png");
					}
					std::printf("%s\n", fix ? (pass ? "  [PASS]" : "  [FAIL]") : "");
				}
			}
		}
		neonGain = 6.0f; eyeAdaptation = 1.0f;
		SetCamera(6.0f);
	}

	// 12. B-13: thin things in front of a spill-lit wall (the user's palm fronds). 23 thin vertical bars (2, 3.5 and 5 units wide, about
	// 1-3 pixels here) 300 units in front of the facade, the game's path, 8 still frames. For the bars' pixels: how many get more
	// than half the light the wall behind them gets (added light, linear luminance), with the spill off, round 5, round 5 with the
	// slope widening zeroed (an edited copy, for measuring only), and NeonSpillHardEdges. Also the ground: its added light and how many
	// of its pixels lose most of it with the key (grain on the street, which the slope term was added for).
	if (part('c')) {
		ID3DXEffect* bounceNoSlope = EditedCopy(fxDir, outDir + "\\fx_noslope", {
			{ "tolerance += 1.5f * slope / NVR_BounceLayout.x;", "" },
			{ "else slope = 1.5f * BlurRadius * min(abs(center.a - before), abs(after - center.a));", "else slope = 0.0f;" } });
		barsOn = true; SetCamera(6.0f);
		const int barBox = (int)StreetBoxes.size();   // the bars come after the street's own boxes
		auto isBar = [&](int i) { return scene.box[i] >= barBox; };
		auto wallBehind = [&](int i) { return scene.box[i] == 2 && scene.emit[i] == 0 && scene.world[i * 3] > -340 && scene.world[i * 3] < 340 &&
			scene.world[i * 3 + 2] > 120 && scene.world[i * 3 + 2] < 520; };
		auto ground = [&](int i) { return scene.box[i] == 0 || scene.box[i] == 1; };
		int bars = 0; for (int i = 0; i < W * H; i++) bars += isBar(i);
		for (const Profile& prof : profiles) {
			shapeFor(prof);
			std::printf("B-13 thin bars in front of a lit wall, %s (%d bar pixels):\n", prof.name, bars);
			std::vector<float> groundR5;
			const char* names[4] = { "spill off", "round 5", "round 5, slope term zeroed", "NeonSpillHardEdges" };
			int litR5 = 0;
			for (int variant = 0; variant < 4; variant++) {
				BounceSettings s = pathFor(prof);
				if (variant == 0) s.spill = 0.0f;
				s.hardEdges = variant == 3 ? 1.0f : 0.0f;
				ClearBounceBuffers(bb);
				for (int f = 0; f < 8; f++) RunBounce(variant == 2 ? bounceNoSlope : bounceNew, bb, s, true, f > 0);
				auto added = AddedLight(bb);
				const double wall = MeanWhere(added, wallBehind), barMean = MeanWhere(added, isBar), groundMean = MeanWhere(added, ground);
				int lit = 0; for (int i = 0; i < W * H; i++) lit += isBar(i) && added[i] > 0.5 * wall;
				if (variant == 1) { groundR5 = added; litR5 = lit; }
				std::printf("  %-28s wall behind %.4f, bars %.4f; bar pixels with more than half the wall's light: %d (%.1f%%); ground %.5f",
					names[variant], wall * 1000, barMean * 1000, lit, 100.0 * lit / max(bars, 1), groundMean);
				if (variant == 3) {
					int lost = 0, groundLit = 0;
					for (int i = 0; i < W * H; i++) if (ground(i) && groundR5[i] > 1e-4f) { groundLit++; lost += added[i] < 0.25f * groundR5[i]; }
					const double change = groundMean / max(MeanWhere(groundR5, ground), 1e-9) - 1.0;
					std::printf(" (%+.1f%% vs round 5); ground pixels losing 75%%+ of round 5's light: %.2f%%  [lit bar pixels at most a third of round 5's,"
						" ground within 5%%, under 1%% of it losing most: %s]", 100.0 * change, 100.0 * lost / max(groundLit, 1),
						tag(lit * 3 <= litR5 && fabs(change) <= 0.05 && lost < 0.01 * max(groundLit, 1)));
				}
				std::printf("\n");
				if (&prof == &profiles[1] && variant >= 1) {
					RunBloom(bloomNew, bb.out.tex, bloomOff, true);
					SavePng(bb.out.tex, 0.04f, 1.0f, exposure, outDir + "\\bars_yours_" + (variant == 1 ? "r5" : variant == 2 ? "noslope" : "hardedges") + ".png");
				}
			}
		}
		barsOn = false; SetCamera(6.0f);
	}

	// 10. B-11: why a stricter light test (LightLevel x2, x4) gave MORE light on the dusk Strip. Street, night and dusk, still camera,
	// the game's path, 8 frames; the mean light added over the picture (linear luminance) for LightLevel x1 / x2 / x4 of each profile:
	// round 5; BrightCap 0; the light-pixel mask off (debug view 2: the same light before Combine's mask); NeonSpillCapSceneLight;
	// NeonSpillMaskSpillOnly; both keys; both keys + NeonSpillSeparateDepth; NeonSpillSeparateDepth alone. Acceptance: with both keys
	// the light never rises from x1 to x2 to x4, day and night.
	if (part('a')) for (int day = 0; day < 2; day++) for (const Profile& prof : profiles) {
		dayLight = day != 0;
		SetCamera(6.0f);
		const char* names[8] = { "round 5", "BrightCap 0", "mask off (no light-pixel test)", "NeonSpillCapSceneLight", "NeonSpillMaskSpillOnly",
			"both keys", "both keys + SeparateDepth", "SeparateDepth alone" };
		std::printf("B-11 LightLevel order, %s, %s (light test at x1 %.3f), light added (x 1000) at LightLevel x1 / x2 / x4:\n", day ? "DUSK" : "NIGHT", prof.name,
			prof.lightLevel * SceneLight());
		for (int variant = 0; variant < 8; variant++) {
			double added[3];
			for (int m = 0; m < 3; m++) {
				shapeFor(prof);
				if (variant == 1) spillShape[2] = 0.0f;
				BounceSettings s = pathFor(prof); s.lightLevel = prof.lightLevel * (float)(1 << m);
				s.capScene = variant == 3 || variant == 5 || variant == 6 ? 1.0f : 0.0f;
				s.maskSpill = variant == 4 || variant == 5 || variant == 6 ? 1.0f : 0.0f;
				s.pyramidFix = variant >= 6 ? 1.0f : 0.0f;
				ClearBounceBuffers(bb);
				for (int f = 0; f < 8; f++) { spillDebug = variant == 2 && f == 7 ? 2 : 0; RunBounce(bounceNew, bb, s, true, f > 0); }
				spillDebug = 0;
				if (variant == 2) {
					auto dbg = ReadF16(bb.out); double sum = 0;
					for (int i = 0; i < W * H; i++) {   // pixels Combine returns untouched (sky, too near, too far) show the scene: nothing added
						const float z = scene.depth[i] * FarZ;
						if (scene.box[i] < 0 || z < 40.0f || z > 6000.0f) continue;
						float d[3]; for (int q = 0; q < 3; q++) d[q] = ToLinear(dbg[i * 4 + q]); sum += Luma(d);
					}
					added[m] = sum / (W * H);
				}
				else { auto a = AddedLight(bb); double sum = 0; for (float x : a) sum += x; added[m] = sum / (W * H); }
			}
			// never rises: each step at most +0.1% (equal is fine: lights far above every light test count the same at all of them)
			const bool falls = added[1] <= added[0] * 1.001 && added[2] <= added[1] * 1.001;
			std::printf("  %-34s %.3f / %.3f / %.3f  %s%s\n", names[variant], added[0] * 1000, added[1] * 1000, added[2] * 1000, falls ? "never rises" : "RISES",
				variant == 5 ? (falls ? "  [acceptance PASS]" : "  [acceptance FAIL]") : "");
		}
		dayLight = false;
	}

	// 6. B-4: issue 3 flux test (the big tube sign and the small lightbox sending the same total light; the ground's spill in rings by
	// distance from each sign's centre, small / big; round 4 shape, the profile's shape, + the fix), and the street floor's added light
	// at Reach 0.25 / 0.5 / 1 (defaults otherwise), in rings by distance from the pink sign, the game's path, 8 still frames.
	if (part('6')) {
		for (const Profile& prof : profiles) {
			LookAt(3, D3DXVECTOR3(0, -300, 220), D3DXVECTOR3(0, 1000, 220));
			std::printf("flux test, %s: the lightbox's pixels are %.1f x as bright as the big sign's tubes (same total light)\n", prof.name, FluxBoxGain);
			const float rings[] = { 360, 450, 600, 800, 1100 };
			for (int shape = 0; shape < 3; shape++) {
				if (shape == 0) { spillShape[0] = 1.0f; spillShape[1] = 1.0f; spillShape[2] = 0.0f; } else shapeFor(prof);
				BounceSettings s = spillOnly(prof); s.pyramidFix = shape == 2 ? 1.0f : 0.0f;
				RunBounce(bounceNew, bb, s, true);
				auto spillMap = SpillAt(bb);
				std::printf("  %s, ground spill small / big (x 1000):", shape == 0 ? "round 4 shape" : shape == 1 ? "this profile's shape" : "+ NeonSpillSeparateDepth");
				for (int r = 0; r + 1 < 5; r++) {
					const double big = MeanWhere(spillMap, [&](int i) { float d = DistTo(i, -600, 1000, 350); return scene.box[i] == 0 && d >= rings[r] && d < rings[r + 1] && scene.world[i * 3] < 0; });
					const double box = MeanWhere(spillMap, [&](int i) { float d = DistTo(i, 600, 1000, 350); return scene.box[i] == 0 && d >= rings[r] && d < rings[r + 1] && scene.world[i * 3] > 0; });
					std::printf("  %g-%g: %.4f / %.4f = %.2f", rings[r], rings[r + 1], box * 1000, big * 1000, box / max(big, 1e-9));
				}
				std::printf("\n");
			}
		}
		SetCamera(6.0f);
		const float rings[] = { 0, 300, 600, 1000, 2000 };
		std::printf("B-4 street floor, defaults with Reach 0.25 / 0.5 / 1, light added (x 1000) on the road and pavement by distance from the pink sign:\n");
		for (float reach : { 0.25f, 0.5f, 1.0f }) {
			shapeFor(profiles[0]);
			BounceSettings s = pathFor(profiles[0]); s.reach = reach;
			ClearBounceBuffers(bb);
			for (int f = 0; f < 8; f++) RunBounce(bounceNew, bb, s, true, f > 0);
			auto added = AddedLight(bb);
			std::printf("  Reach %.2f: whole floor %.3f;", reach, MeanWhere(added, [&](int i) { return scene.box[i] == 0 || scene.box[i] == 1; }) * 1000);
			for (int r = 0; r + 1 < 5; r++)
				std::printf("  %g-%g: %.3f", rings[r], rings[r + 1], MeanWhere(added, [&](int i) { float d = DistTo(i, 420, 650, 400);
					return (scene.box[i] == 0 || scene.box[i] == 1) && d >= rings[r] && d < rings[r + 1]; }) * 1000);
			std::printf("\n");
		}
	}

	// 7. B-5: sweep then STOP on the game's path, the spill alone (Strength 0): the camera turns left at 90 degrees a second for 0.4 s
	// (the pink sign leaves on the right), then holds still for 1.6 s (NeonSpillHoldAfterTurn 1 s ends 1 s after the stop), at 60
	// and at 30 frames a second (M6: the hold must last the same in seconds). Followed: light added (red, linear) on road spots that
	// stay in view, as a share of the first frame's, at times after the stop; and the time after the stop until it is below 50% of
	// its value at the stop.
	auto sweep = [&](const Profile& prof, ID3DXEffect* e, float holdAfter, const char* label, int fps) {
		shapeFor(prof);
		frameSeconds = 1.0f / fps;
		BounceSettings s = pathFor(prof); s.holdAfter = holdAfter; s.strength = 0.0f;
		const D3DXVECTOR3 spots[] = { { 0, 900, 0 }, { 100, 700, 0 }, { 150, 1100, 0 } };
		const int turnFrames = (int)(0.4f * fps + 0.5f), frames = (int)(2.0f * fps + 0.5f);
		std::vector<std::vector<float>> trace(3);
		ClearBounceBuffers(bb);
		for (int f = 0; f < frames; f++) {
			SetCamera(6.0f - 90.0f / fps * (float)min(f, turnFrames));
			RunBounce(e, bb, s, true, f > 0);
			auto out = ReadF16(bb.out);
			for (int k = 0; k < 3; k++) {
				D3DXVECTOR3 v; D3DXVec3TransformCoord(&v, &spots[k], &view);
				const float u = (v.x * proj._11 / v.z) * 0.5f + 0.5f, w = 0.5f - (v.y * proj._22 / v.z) * 0.5f;
				const int x = (int)(u * W), y = (int)(w * H);
				float r = -1.0f;
				if (v.z > 0 && x >= 2 && x < W - 2 && y >= 2 && y < H - 2) {
					r = 0; for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) { const int i = (y + dy) * W + x + dx; r += ToLinear(out[i * 4]) - ToLinear(scene.colour[i * 4]); }
					r /= 25.0f;
				}
				trace[k].push_back(r);
			}
		}
		frameSeconds = 1.0f / 60.0f;
		auto at = [&](int k, float secondsAfterStop) { const int f = min(frames - 1, turnFrames + (int)(secondsAfterStop * fps + 0.5f)); return trace[k][f] / trace[k][0]; };
		std::printf("sweep then stop, %s, %s, %d fps (the turn stops at 0.4 s):\n", prof.name, label, fps);
		for (int k = 0; k < 3; k++) {
			const float atStop = trace[k][turnFrames];
			float below = -1.0f;
			for (int f = turnFrames + 1; f < frames; f++) if (below < 0 && trace[k][f] >= 0 && trace[k][f] < 0.5f * atStop) below = (f - turnFrames) / (float)fps;
			std::printf("  spot %d: first frame %.4f; at the stop %.2f; after the stop 0.1/0.25/0.5/1.0/1.5 s: %.2f %.2f %.2f %.2f %.2f; below 50%% of the stop value after %.2f s\n",
				k, trace[k][0], atStop / trace[k][0], at(k, 0.1f), at(k, 0.25f), at(k, 0.5f), at(k, 1.0f), at(k, 1.5f), below);
		}
	};
	if (part('7')) for (const Profile& prof : profiles) for (int fps : { 60, 30 }) {
		sweep(prof, bounceR5, 0.0f, "round 5", fps); sweep(prof, bounceNew, 1.0f, "NeonSpillHoldAfterTurn 1", fps);
	}

	// 11. B-12 (measure only): static flicker round a small very bright emitter (the 27x lightbox) and a big dim one (the tube sign),
	// the flux scene, the game's path, 30 settle + 60 measured frames; steady signs, then both signs flickering +-5% a frame. Spill
	// off is the reference. Per area (within 800 units of each sign, not the signs themselves): mean temporal std of the final image
	// (linear luminance, x 1000), the share of "flickering" pixels (std above 2x the reference's and above 0.5% of its brightness) and
	// the light added. Round 5, each key, all keys.
	if (part('b')) for (int animated = 0; animated < 2; animated++) for (const Profile& prof : profiles) {
		LookAt(3, D3DXVECTOR3(0, -300, 220), D3DXVECTOR3(0, 1000, 220));
		auto measure = [&](const BounceSettings& s, std::vector<float>& stdv, std::vector<float>& mean) {
			ClearBounceBuffers(bb);
			std::vector<double> sum(W * H, 0.0), sum2(W * H, 0.0);
			for (int f = 0; f < 90; f++) {
				if (animated) {   // both signs flicker +-5% a frame, independently (a buzzing tube, an animated sign)
					fluxFlicker[0] = 1.0f + 0.05f * (Hash((float)f, 1.3f) * 2.0f - 1.0f); fluxFlicker[1] = 1.0f + 0.05f * (Hash((float)f, 7.9f) * 2.0f - 1.0f);
					LookAt(3, D3DXVECTOR3(0, -300, 220), D3DXVECTOR3(0, 1000, 220));
				}
				RunBounce(bounceNew, bb, s, true, f > 0);
				if (f < 30) continue;
				auto out = ReadF16(bb.out);
				for (int i = 0; i < W * H; i++) { float o[3]; for (int q = 0; q < 3; q++) o[q] = ToLinear(out[i * 4 + q]); const double l = Luma(o); sum[i] += l; sum2[i] += l * l; }
			}
			stdv.assign(W * H, 0.0f); mean.assign(W * H, 0.0f);
			for (int i = 0; i < W * H; i++) { const double m = sum[i] / 60; mean[i] = (float)m; stdv[i] = (float)sqrt(max(0.0, sum2[i] / 60 - m * m)); }
		};
		shapeFor(prof);
		std::vector<float> stdOff, meanOff;
		BounceSettings off = pathFor(prof); off.spill = 0.0f;
		measure(off, stdOff, meanOff);
		auto nearSmall = [&](int i) { return scene.box[i] >= 0 && scene.emit[i] == 0 && DistTo(i, 600, 1000, 350) < 800; };
		auto nearBig = [&](int i) { return scene.box[i] >= 0 && scene.emit[i] == 0 && DistTo(i, -600, 1000, 350) < 800; };
		std::printf("B-12 flicker, %s, %s: spill off: small %.4f, big %.4f (mean std x 1000)\n", animated ? "both signs flickering +-5%" : "steady signs",
			prof.name, MeanWhere(stdOff, nearSmall) * 1000, MeanWhere(stdOff, nearBig) * 1000);
		const char* names[6] = { "round 5", "+ SeparateDepth", "+ CapSceneLight", "+ MaskSpillOnly", "+ HardEdges", "all four keys" };
		for (int variant = 0; variant < 6; variant++) {
			BounceSettings s = pathFor(prof);
			s.pyramidFix = variant == 1 || variant == 5 ? 1.0f : 0.0f;
			s.capScene = variant == 2 || variant == 5 ? 1.0f : 0.0f;
			s.maskSpill = variant == 3 || variant == 5 ? 1.0f : 0.0f;
			s.hardEdges = variant == 4 || variant == 5 ? 1.0f : 0.0f;
			std::vector<float> stdv, mean;
			measure(s, stdv, mean);
			auto flicker = [&](int i) { return stdv[i] > 2.0f * stdOff[i] && stdv[i] > 0.005f * meanOff[i] + 1e-6f; };
			int fs = 0, ns = 0, fb = 0, nb = 0;
			for (int i = 0; i < W * H; i++) { if (nearSmall(i)) { ns++; fs += flicker(i); } if (nearBig(i)) { nb++; fb += flicker(i); } }
			std::printf("  %-16s small: std %.4f, flickering %.1f%%, light added %.4f | big: std %.4f, flickering %.1f%%, light added %.4f\n", names[variant],
				MeanWhere(stdv, nearSmall) * 1000, 100.0 * fs / max(ns, 1), (MeanWhere(mean, nearSmall) - MeanWhere(meanOff, nearSmall)) * 1000,
				MeanWhere(stdv, nearBig) * 1000, 100.0 * fb / max(nb, 1), (MeanWhere(mean, nearBig) - MeanWhere(meanOff, nearBig)) * 1000);
		}
		fluxFlicker[0] = fluxFlicker[1] = 1.0f;
		SetCamera(6.0f);
	}

	// 14. B-14 (measure only, critic review 7): the light the picture gains just from switching the spill ON, C, against the light per
	// unit of NeonSpill, k: total picture light L (linear luminance, mean over the picture) after the game's path (8 still frames, the
	// street at night, the game's light); C = L(NeonSpill 0.01) - L(off), k = (L(NeonSpill 1) - L(0.01)) / 0.99, both as % of L(off).
	// Split by taking each source out in turn (edited copies, measuring only): Blur's and Combine's slope tolerance, round 5's
	// light-pixel mask, MultiBounce (0 in both runs); and with the round-6 keys. (Specular's merged combine, used with the spill off in
	// game, is not in the preview; its AddBounce is Combine's spill-off sums, see the report.)
	if (part('e')) {
		ID3DXEffect* noSlope = EditedCopy(fxDir, outDir + "\\fx_b14_noslope", {
			{ "tolerance += 1.5f * slope / NVR_BounceLayout.x;", "" },
			{ "else slope = 1.5f * BlurRadius * min(abs(center.a - before), abs(after - center.a));", "else slope = 0.0f;" } });
		ID3DXEffect* noMask = EditedCopy(fxDir, outDir + "\\fx_b14_nomask", { {
			"[branch] if (NVR_BounceSpill.x > 0.0f && NVR_SpillOptions.z <= 0.0f) indirect *= 1.0f - SpillAmount(lin, max(NVR_SpillMemory.w, 0.002f));", "" } });
		ID3DXEffect* neither = EditedCopy(fxDir, outDir + "\\fx_b14_neither", {
			{ "tolerance += 1.5f * slope / NVR_BounceLayout.x;", "" },
			{ "else slope = 1.5f * BlurRadius * min(abs(center.a - before), abs(after - center.a));", "else slope = 0.0f;" },
			{ "[branch] if (NVR_BounceSpill.x > 0.0f && NVR_SpillOptions.z <= 0.0f) indirect *= 1.0f - SpillAmount(lin, max(NVR_SpillMemory.w, 0.002f));", "" } });
		SetCamera(6.0f);
		auto pictureLight = [&](ID3DXEffect* e, const BounceSettings& s) {
			ClearBounceBuffers(bb);
			for (int f = 0; f < 8; f++) RunBounce(e, bb, s, true, f > 0);
			auto out = ReadF16(bb.out); double sum = 0;
			for (int i = 0; i < W * H; i++) { float o[3]; for (int q = 0; q < 3; q++) o[q] = ToLinear(out[i * 4 + q]); sum += Luma(o); }
			return sum / (W * H);
		};
		for (const Profile& prof : profiles) {
			shapeFor(prof);
			std::printf("B-14 C (switching the spill on) and k (per unit of NeonSpill), %s, street at night:\n", prof.name);
			struct Case { const char* name; ID3DXEffect* e; float multiBounce, sep, cap, mask, hard; };
			const Case cases[] = {
				{ "round 5", bounceNew, 1, 0, 0, 0, 0 },
				{ "without the slope tolerance", noSlope, 1, 0, 0, 0, 0 },
				{ "without round 5's mask", noMask, 1, 0, 0, 0, 0 },
				{ "without slope and mask", neither, 1, 0, 0, 0, 0 },
				{ "MultiBounce 0", bounceNew, 0, 0, 0, 0, 0 },
				{ "+ NeonSpillSeparateDepth", bounceNew, 1, 1, 0, 0, 0 },
				{ "+ NeonSpillCapSceneLight", bounceNew, 1, 0, 1, 0, 0 },
				{ "+ NeonSpillMaskSpillOnly", bounceNew, 1, 0, 0, 1, 0 },
				{ "+ NeonSpillHardEdges", bounceNew, 1, 0, 0, 0, 1 },
				{ "all four keys", bounceNew, 1, 1, 1, 1, 1 } };
			for (const Case& c : cases) {
				BounceSettings s = pathFor(prof); s.multiBounce = c.multiBounce;
				s.pyramidFix = c.sep; s.capScene = c.cap; s.maskSpill = c.mask; s.hardEdges = c.hard;
				BounceSettings off = s; off.spill = 0.0f;
				BounceSettings eps = s; eps.spill = 0.01f;
				BounceSettings one = s; one.spill = 1.0f;
				const double lOff = pictureLight(c.e, off), lEps = pictureLight(c.e, eps), lOne = pictureLight(c.e, one);
				const double C = (lEps - lOff) / lOff, k = (lOne - lEps) / 0.99 / lOff;
				std::printf("  %-30s C %+.2f%%, k %+.2f%% (C = %+.2f k); half / full at the profile's NeonSpill: %.2f\n", c.name, 100.0 * C, 100.0 * k, C / max(fabs(k), 1e-9),
					(C + 0.5 * prof.spill * k) / max(C + prof.spill * k, 1e-9));
			}
		}
	}

	// 15. B-2 stand-off lights (critic notes 9a): lights that stand OFF the wall must still light it. The coplanar scene's wall with no
	// painted sign, and instead (a) three tube bars 6-10 units in front of it (x 140-260, at z 170 / 200 / 230), or (b) a canopy strip
	// 40 units deep sticking out of the wall at z 300. The spill alone (one still frame) on the wall by distance from the lights'
	// footprint on it, round 5, NeonSpillSeparateDepth, and NeonSpillSeparateDepth without its plane test (an edited copy, measuring
	// only). Pass: the key within +-15% of round 5 on the wall within 100 units.
	if (part('f')) {
		ID3DXEffect* noBand = EditedCopy(fxDir, outDir + "\\fx_noband", { { "static const float SpillPlaneSpread = 0.5f;", "static const float SpillPlaneSpread = 0.0f;" } });
		for (int mode = 1; mode <= 2; mode++) {
			standoffMode = mode;
			const float rx0 = mode == 1 ? 140.0f : 100.0f, rx1 = mode == 1 ? 260.0f : 300.0f, rz0 = mode == 1 ? 168.0f : 300.0f, rz1 = mode == 1 ? 232.0f : 304.0f;
			auto footprint = [&](int i) {
				const float dx = max(0.0f, max(rx0 - scene.world[i * 3], scene.world[i * 3] - rx1));
				const float dz = max(0.0f, max(rz0 - scene.world[i * 3 + 2], scene.world[i * 3 + 2] - rz1));
				return sqrtf(dx * dx + dz * dz); };
			for (int view = 0; view < 2; view++) {
				if (view == 0) LookAt(2, D3DXVECTOR3(-500, 350, 170), D3DXVECTOR3(150, 1000, 200));
				else LookAt(2, D3DXVECTOR3(60, 250, 180), D3DXVECTOR3(60, 1000, 180));
				for (const Profile& prof : profiles) {
					shapeFor(prof);
					const float rings[] = { 0, 40, 100, 200 };
					double v[3][3] = {};
					for (int variant = 0; variant < 3; variant++) {
						BounceSettings s = spillOnly(prof); s.pyramidFix = variant ? 1.0f : 0.0f;
						RunBounce(variant == 2 ? noBand : bounceNew, bb, s, true);
						auto spillMap = SpillAt(bb);
						for (int r = 0; r < 3; r++) v[variant][r] = MeanWhere(spillMap, [&](int i) { const float d = footprint(i);
							return scene.box[i] == 1 && scene.world[i * 3 + 1] < 1000.5f && scene.emit[i] == 0 && d >= rings[r] && d < rings[r + 1]; });
					}
					const double r5 = v[0][0] + v[0][1], key = v[1][0] + v[1][1];
					std::printf("stand-off %s, %s view, %s: wall 0-40 / 40-100 / 100-200 units (x 1000): round 5 %.3f %.3f %.3f; SeparateDepth %.3f %.3f %.3f;"
						" without its plane test %.3f %.3f %.3f  [within 100 units: %+.0f%%: %s]\n", mode == 1 ? "tubes 6-10 units off the wall" : "canopy strip",
						view ? "straight-on" : "oblique", prof.name, v[0][0] * 1000, v[0][1] * 1000, v[0][2] * 1000, v[1][0] * 1000, v[1][1] * 1000, v[1][2] * 1000,
						v[2][0] * 1000, v[2][1] * 1000, v[2][2] * 1000, 100.0 * (key / max(r5, 1e-12) - 1.0), tag(fabs(key / max(r5, 1e-12) - 1.0) <= 0.15));
				}
			}
		}
		standoffMode = 0; SetCamera(6.0f);
	}

	// 9. GPU time at this resolution (fastest of 6 interleaved rounds), the game's path at the defaults: spill off, round 5, each key on.
	if (part('9')) {
		SetCamera(6.0f);
		shapeFor(profiles[0]);
		BounceSettings off = pathFor(profiles[0]), on = off, hold = off, sep = off, cap = off, mask = off, hard = off, all = off;
		off.spill = 0.0f;
		hold.holdAfter = 1.0f; sep.pyramidFix = 1.0f; cap.capScene = 1.0f; mask.maskSpill = 1.0f; hard.hardEdges = 1.0f;
		all.holdAfter = all.pyramidFix = all.capScene = all.maskSpill = all.hardEdges = 1.0f;
		const BounceSettings* cases[8] = { &off, &on, &hold, &sep, &cap, &mask, &hard, &all };
		const char* names[8] = { "spill off", "NeonSpill 1 (round 5)", "+ NeonSpillHoldAfterTurn 1", "+ NeonSpillSeparateDepth 1", "+ NeonSpillCapSceneLight 1",
			"+ NeonSpillMaskSpillOnly 1", "+ NeonSpillHardEdges 1", "all five keys" };
		double best[10]; for (int k = 0; k < 10; k++) best[k] = 1e9;
		for (int round = 0; round < 6; round++) {
			for (int k = 0; k < 8; k++) best[k] = min(best[k], GpuTime([&] { RunBounce(bounceNew, bb, *cases[k], true, true); }, 20));
			best[8] = min(best[8], GpuTime([&] { RunBloom(bloomOld, sceneTex, bloomOff, false); }, 20));
			best[9] = min(best[9], GpuTime([&] { RunBloom(bloomNew, sceneTex, bloomOff, true); }, 20));
		}
		for (int k = 0; k < 8; k++) std::printf("GPU %dx%d, game's path, defaults: %s %.3f ms (%+.3f vs spill off)\n", W, H, names[k], best[k], best[k] - best[0]);
		std::printf("GPU %dx%d: bloom old file %.3f ms, new file all off %.3f ms\n", W, H, best[8], best[9]);
	}
	std::printf(failures ? "%d check(s) FAILED\n" : "All checks passed\n", failures);
	return failures ? 1 : 0;
}
