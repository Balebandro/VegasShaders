// Preview of the BloodFlow effect without the game: a small scene (depth, normals and colour made here by ray casting:
// floor, back wall, a ledge on the wall, a floating table top (an overhang), a steep ramp and a gentle mound), the game's
// blood splats painted on it, then the real BloodFlow.fx.hlsl drawn over it with the real box and probe code
// (src\effects\BloodFlowBoxes.h), the way NVR does it: the probe pass into a ping-pong path texture, then the boxes.
// Writes PNGs per age, a debug view, and a camera-move check (paths found from one camera, kept, drawn from another).
// Usage: build\blood-test\blood_preview.exe <path to BloodFlow.fx.hlsl> <output folder> [close | terrain]
// terrain (2026-10-13): rolling desert ground instead (gentle 3-15 degree slopes, as most of the Mojave), blood thrown onto it
// as the game reports ground splats (thrown straight down), two pools on slopes; writes a 20 s time-lapse (terrain_NNN.png,
// every 0.5 s, plus the same in the debug view) and a check per splat: did its streams run downhill, and how far.
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/effects/BloodFlowBoxes.h"

static const int W = 1280, H = 720;
static const float NearZ = 5.0f, FarZ = 100000.0f;
static const int ItemMax = 96, PathRows = ItemMax * BloodFlowBoxes::Streams;

static float Hash(float x, float y) { float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f; return h - floorf(h); }
static float Noise(float x, float y) {
	float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
	float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
	return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
static float Fbm(float x, float y) { return 0.5f * Noise(x, y) + 0.3f * Noise(x * 2.1f, y * 2.1f) + 0.2f * Noise(x * 4.3f, y * 4.3f); }
static float ToGamma(float c) { c = c < 0 ? 0 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAILED %s: 0x%08lX (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

// ---- The scene ----------------------------------------------------------------------------------------------------
struct Hit { float t; D3DXVECTOR3 n; int material; };	// material: 0 floor, 1 wall, 2 wood, 3 concrete block

struct Box { D3DXVECTOR3 lo, hi; int material; };
static const Box Boxes[] = {
	{ D3DXVECTOR3(-230, 268, 56), D3DXVECTOR3(-50, 300, 64), 2 },	// a ledge on the wall
	{ D3DXVECTOR3(0, 150, 62), D3DXVECTOR3(120, 200, 100), 2 },	// a table top floating above the floor (an overhang)
};
// Ramps: z = sx * (x - x0) + sy * (y - y0) over x0..x1, y0..y1 (solid under it, down to the floor).
struct Ramp { float x0, x1, y0, y1, sx, sy; int material; };
static const Ramp Ramps[] = {
	{ 135, 260, 110, 250, 0.25f, 0.75f, 3 },	// steep (about 40 degrees), falling towards the camera and left
	{ -240, -100, 60, 200, 0.0f, 0.25f, 3 },	// a gentle mound (14 degrees), falling towards the camera
};
static const float WallY = 300.0f;

static float RampZ(const Ramp& r, float x, float y) { return r.sx * (x - r.x0) + r.sy * (y - r.y0); }

// Terrain mode: a height field. Falls towards the camera (-y) at about 6 degrees on average, with long swells and small
// bumps, so slopes run from about 2 to 15 degrees; a back wall stands at WallY. Material 0 (ground).
static bool TerrainMode = false;
// Rocks (2026-10-13, BloodFlow rewrite): smooth bumps a few units high just downhill of some splats; the streams must go
// round them, not over (the obstacle check). x, y, radius, height.
static const float Rocks[][4] = { { 60, 24, 6, 5 }, { -120, 44, 6, 5 }, { -30, 86, 7, 5 } };
static bool WithRocks = true;
static float RockZ(float x, float y) {
	float z = 0.0f;
	if (WithRocks) for (const auto& r : Rocks) { const float u = ((x - r[0]) * (x - r[0]) + (y - r[1]) * (y - r[1])) / (r[2] * r[2]); if (u < 1) z += r[3] * (1 - u) * (1 - u); }
	return z;
}
static float GroundZ(float x, float y) {
	return 0.105f * y + 9.0f * sinf(x * 0.011f + 0.6f) * cosf(y * 0.008f) + 14.0f * (Fbm(x * 0.006f + 4.0f, y * 0.006f) - 0.5f) + RockZ(x, y);
}
static D3DXVECTOR3 GroundNormal(float x, float y) {
	const float e = 0.5f;
	D3DXVECTOR3 n(-(GroundZ(x + e, y) - GroundZ(x - e, y)) / (2 * e), -(GroundZ(x, y + e) - GroundZ(x, y - e)) / (2 * e), 1.0f);
	D3DXVec3Normalize(&n, &n);
	return n;
}
static Hit CastTerrain(const D3DXVECTOR3& o, const D3DXVECTOR3& d) {
	Hit h = { 1e9f, D3DXVECTOR3(0, 0, 1), -1 };
	if (d.y > 0) { const float t = (WallY - o.y) / d.y; const D3DXVECTOR3 p = o + d * t; if (t > 0 && p.z > GroundZ(p.x, p.y)) { h.t = t; h.n = D3DXVECTOR3(0, -1, 0); h.material = 1; } }
	float prev = 0.0f;
	for (float t = 1.0f; t < min(h.t, 6000.0f); t += 1.0f + t * 0.004f) {
		const D3DXVECTOR3 p = o + d * t;
		if (p.z < GroundZ(p.x, p.y)) {
			float a = prev, b = t;
			for (int i = 0; i < 12; i++) { const float m = 0.5f * (a + b); const D3DXVECTOR3 q = o + d * m; if (q.z < GroundZ(q.x, q.y)) b = m; else a = m; }
			const D3DXVECTOR3 q = o + d * b;
			h.t = b; h.n = GroundNormal(q.x, q.y); h.material = 0;
			break;
		}
		prev = t;
	}
	return h;
}

static Hit Cast(const D3DXVECTOR3& o, const D3DXVECTOR3& d) {
	if (TerrainMode) return CastTerrain(o, d);
	Hit h = { 1e9f, D3DXVECTOR3(0, 0, 1), -1 };
	if (d.z < 0) { const float t = -o.z / d.z; if (t > 0 && t < h.t) { h.t = t; h.n = D3DXVECTOR3(0, 0, 1); h.material = 0; } }
	if (d.y > 0) { const float t = (WallY - o.y) / d.y; if (t > 0 && t < h.t) { h.t = t; h.n = D3DXVECTOR3(0, -1, 0); h.material = 1; } }
	for (const Box& b : Boxes) {
		float t0 = 0, t1 = 1e9f, sign = 0;
		int axis = -1;
		bool miss = false;
		for (int a = 0; a < 3 && !miss; a++) {
			const float oa = (&o.x)[a], da = (&d.x)[a], lo = (&b.lo.x)[a], hi = (&b.hi.x)[a];
			if (fabsf(da) < 1e-8f) { if (oa < lo || oa > hi) miss = true; continue; }
			float ta = (lo - oa) / da, tb = (hi - oa) / da;
			float s = -1;
			if (ta > tb) { std::swap(ta, tb); s = 1; }
			if (ta > t0) { t0 = ta; axis = a; sign = s; }
			if (tb < t1) t1 = tb;
			if (t0 > t1) miss = true;
		}
		if (!miss && axis >= 0 && t0 > 0 && t0 < h.t) { h.t = t0; h.n = D3DXVECTOR3(0, 0, 0); (&h.n.x)[axis] = sign; h.material = b.material; }
	}
	for (const Ramp& r : Ramps) {
		D3DXVECTOR3 n(-r.sx, -r.sy, 1.0f);
		D3DXVec3Normalize(&n, &n);
		const D3DXVECTOR3 p0(r.x0, r.y0, 0.0f);
		const float dn = D3DXVec3Dot(&d, &n);
		if (dn < 0) {
			const D3DXVECTOR3 toP0 = p0 - o;
			const float t = D3DXVec3Dot(&toP0, &n) / dn;
			const D3DXVECTOR3 p = o + d * t;
			if (t > 0 && t < h.t && p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1 && p.z >= 0) { h.t = t; h.n = n; h.material = r.material; }
		}
		auto under = [&](const D3DXVECTOR3& p) { return p.z >= 0 && p.z <= RampZ(r, p.x, p.y); };
		if (d.y > 0) {
			const float t = (r.y0 - o.y) / d.y;
			const D3DXVECTOR3 p = o + d * t;
			if (t > 0 && t < h.t && p.x >= r.x0 && p.x <= r.x1 && under(p)) { h.t = t; h.n = D3DXVECTOR3(0, -1, 0); h.material = r.material; }
		}
		if (d.x > 0) {
			const float t = (r.x0 - o.x) / d.x;
			const D3DXVECTOR3 p = o + d * t;
			if (t > 0 && t < h.t && p.y >= r.y0 && p.y <= r.y1 && under(p)) { h.t = t; h.n = D3DXVECTOR3(-1, 0, 0); h.material = r.material; }
		}
		if (d.x < 0) {
			const float t = (r.x1 - o.x) / d.x;
			const D3DXVECTOR3 p = o + d * t;
			if (t > 0 && t < h.t && p.y >= r.y0 && p.y <= r.y1 && under(p)) { h.t = t; h.n = D3DXVECTOR3(1, 0, 0); h.material = r.material; }
		}
	}
	return h;
}

// The game's splats (painted into the image) and BloodFlow's items for them. hint: the guessed normal BloodFlow.cpp
// sends (against the way the blood was thrown), deliberately a little off.
struct Splat { D3DXVECTOR3 at; D3DXVECTOR3 hint; float r; };
static std::vector<Splat> MakeSplats() {
	std::vector<Splat> s;
	if (TerrainMode) {	// on the ground, thrown straight down (the game's "thrown (0 0 1)"), the hint a little off
		const float at[][3] = { { -120, 60, 13 }, { -30, 110, 11 }, { 60, 40, 14 }, { 140, 150, 12 }, { -170, 190, 10 }, { 20, 220, 12 } };
		for (const auto& a : at) s.push_back({ D3DXVECTOR3(a[0], a[1], GroundZ(a[0], a[1])), D3DXVECTOR3(0.05f, -0.05f, 1.0f), a[2] });
		return s;
	}
	s.push_back({ D3DXVECTOR3(-140, WallY, 95), D3DXVECTOR3(0.1f, -1, 0.05f), 12 });		// wall above the ledge
	s.push_back({ D3DXVECTOR3(-10, WallY, 34), D3DXVECTOR3(-0.2f, -1, 0.1f), 14 });		// low on the wall, runs to the floor
	s.push_back({ D3DXVECTOR3(70, 150, 90), D3DXVECTOR3(0.25f, -1, 0.15f), 10 });		// table's front face, drips off its edge
	s.push_back({ D3DXVECTOR3(195, 135, RampZ(Ramps[0], 195, 135)), D3DXVECTOR3(0.2f, -0.6f, 0.7f), 12 });	// the steep ramp
	s.push_back({ D3DXVECTOR3(60, 95, 0), D3DXVECTOR3(0, 0, 1), 10 });					// on the floor: stays put
	return s;
}

struct Camera { D3DXVECTOR3 eye, F, R, U; D3DXMATRIX view, proj, invView, invProj, viewProj; };
static Camera MakeCamera(const D3DXVECTOR3& eye, float yawDeg, float pitchDeg) {
	Camera c;
	c.eye = eye;
	const float yaw = D3DXToRadian(yawDeg), pitch = D3DXToRadian(pitchDeg);
	c.F = D3DXVECTOR3(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), -sinf(pitch));
	D3DXVECTOR3 Up(0, 0, 1);
	D3DXVec3Cross(&c.R, &c.F, &Up); D3DXVec3Normalize(&c.R, &c.R);
	D3DXVec3Cross(&c.U, &c.R, &c.F);
	D3DXMatrixIdentity(&c.view);
	c.view._11 = c.R.x; c.view._12 = c.U.x; c.view._13 = c.F.x;
	c.view._21 = c.R.y; c.view._22 = c.U.y; c.view._23 = c.F.y;
	c.view._31 = c.R.z; c.view._32 = c.U.z; c.view._33 = c.F.z;
	D3DXMatrixTranspose(&c.invView, &c.view);
	const float fovY = D3DXToRadian(55.0f), aspect = (float)W / H;
	D3DXMatrixIdentity(&c.proj);
	c.proj._11 = 1.0f / (tanf(fovY * 0.5f) * aspect); c.proj._22 = 1.0f / tanf(fovY * 0.5f);
	c.proj._33 = FarZ / (FarZ - NearZ); c.proj._34 = 1.0f; c.proj._43 = -NearZ * FarZ / (FarZ - NearZ); c.proj._44 = 0.0f;
	D3DXMatrixInverse(&c.invProj, NULL, &c.proj);
	c.viewProj = c.view * c.proj;
	return c;
}

int main(int argc, char** argv) {
	if (argc < 3) { std::puts("usage: blood_preview <BloodFlow.fx.hlsl> <output folder> [close]"); return 2; }
	const std::string outDir = argv[2];
	const bool close = argc > 3 && !strcmp(argv[3], "close");
	TerrainMode = argc > 3 && !strcmp(argv[3], "terrain");
	// "bars" (2026-10-13), after the mode: the earlier drawn trails (BLOOD_BARS) instead of WetWorld's rivulets, for comparison
	// (shaders before the rewrite). After the rewrite: "norocks" (terrain without the rocks), "amount=0.4", "visc=2",
	// "speed=1.5" (BloodAmount, Viscosity, FlowSpeed; defaults 0.75, 1, 1).
	bool bars = false, cutsMode = false;
	float amount = 0.75f, viscosity = 1.0f, flowSpeed = 1.0f;
	for (int i = 3; i < argc; i++) {
		bars |= !strcmp(argv[i], "bars");
		cutsMode |= !strcmp(argv[i], "cuts");
		if (!strcmp(argv[i], "norocks")) WithRocks = false;
		sscanf_s(argv[i], "amount=%f", &amount); sscanf_s(argv[i], "visc=%f", &viscosity); sscanf_s(argv[i], "speed=%f", &flowSpeed);
	}

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "BloodPreview"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("BloodPreview", "", WS_OVERLAPPEDWINDOW, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice9* device = nullptr;
	CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));

	const D3DXVECTOR3 sun = [] { D3DXVECTOR3 s(0.35f, -0.65f, 0.68f); D3DXVec3Normalize(&s, &s); return s; }();
	const std::vector<Splat> splats = MakeSplats();

	IDirect3DTexture9 *depthTex, *normalTex, *shadowTex, *viewModelTex, *imageTex, *sourceTex, *sceneTex, *pathTex[2], *pathRead;
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &depthTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &normalTex, NULL));
	CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_G16R16, D3DPOOL_MANAGED, &shadowTex, NULL));
	CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &viewModelTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &imageTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &sourceTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sceneTex, NULL));
	for (int i = 0; i < 2; i++) CHECK(device->CreateTexture(BloodFlowBoxes::PathColumns, PathRows, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &pathTex[i], NULL));
	CHECK(device->CreateTexture(BloodFlowBoxes::PathColumns, PathRows, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &pathRead, NULL));
	IDirect3DTexture9* boundsTex;
	IDirect3DSurface9* boundsSurface;
	CHECK(device->CreateTexture(2, ItemMax, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &boundsTex, NULL));
	boundsTex->GetSurfaceLevel(0, &boundsSurface);
	D3DLOCKED_RECT lr;
	CHECK(shadowTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] = 0x0000FFFF;   // r = 1: in the sun
	shadowTex->UnlockRect(0);
	CHECK(viewModelTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((float*)((char*)lr.pBits + y * lr.Pitch))[x] = 1.0f;          // no weapon
	viewModelTex->UnlockRect(0);

	// Renders the scene from a camera into the depth, normal and scene textures.
	auto renderScene = [&](const Camera& cam) -> int {
		std::vector<float> depth(W * H), normals(W * H * 4), colour(W * H * 3);
		for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
			const float nx = (px + 0.5f) / W * 2 - 1, ny = 1 - (py + 0.5f) / H * 2;
			const D3DXVECTOR3 dir = cam.F + cam.R * (nx / cam.proj._11) + cam.U * (ny / cam.proj._22);
			const Hit h = Cast(cam.eye, dir);
			const int i = py * W + px;
			if (h.material < 0) { depth[i] = 1.0f; colour[i * 3] = 0.5f; colour[i * 3 + 1] = 0.6f; colour[i * 3 + 2] = 0.8f; continue; }
			const D3DXVECTOR3 p = cam.eye + dir * h.t;
			depth[i] = h.t / FarZ;	// dir's forward component is 1: t is the view depth
			normals[i * 4 + 0] = D3DXVec3Dot(&h.n, &cam.R) * 0.5f + 0.5f;
			normals[i * 4 + 1] = D3DXVec3Dot(&h.n, &cam.U) * 0.5f + 0.5f;
			normals[i * 4 + 2] = D3DXVec3Dot(&h.n, &cam.F) * 0.5f + 0.5f;
			normals[i * 4 + 3] = 1.0f;
			float albedo[3];
			if (h.material == 1) { float v = 0.42f + 0.18f * Fbm(p.x * 0.05f, p.z * 0.05f); albedo[0] = v; albedo[1] = v * 0.98f; albedo[2] = v * 0.95f; }
			else if (h.material == 0) { float v = 0.75f + 0.35f * Fbm(p.x * 0.04f + 9, p.y * 0.04f); albedo[0] = 0.45f * v; albedo[1] = 0.36f * v; albedo[2] = 0.26f * v; }
			else if (h.material == 2) { float v = 0.8f + 0.3f * Noise(p.x * 0.02f + p.y * 0.02f, p.z * 0.6f); albedo[0] = 0.40f * v; albedo[1] = 0.27f * v; albedo[2] = 0.16f * v; }
			else { float v = 0.36f + 0.16f * Fbm(p.x * 0.07f + 3, p.y * 0.07f + p.z * 0.05f); albedo[0] = v; albedo[1] = v * 0.97f; albedo[2] = v * 0.93f; }
			const float lightLevel = 1.1f * max(0.0f, D3DXVec3Dot(&h.n, &sun)) + 0.3f;
			float c[3] = { albedo[0] * lightLevel, albedo[1] * lightLevel, albedo[2] * lightLevel };
			for (const Splat& s : splats) {
				const D3DXVECTOR3 off = p - s.at;
				const float dist = D3DXVec3Length(&off);
				if (dist > s.r * 1.2f) continue;
				const float a = atan2f(off.z + off.y, off.x);
				const float edge = s.r * (0.65f + 0.35f * Fbm(cosf(a) * 2 + s.at.x, sinf(a) * 2 + s.at.z));
				const float mask = 1.0f - min(1.0f, max(0.0f, (dist - edge) / 1.0f));
				c[0] += (0.30f * lightLevel - c[0]) * mask; c[1] += (0.015f * lightLevel - c[1]) * mask; c[2] += (0.012f * lightLevel - c[2]) * mask;
			}
			colour[i * 3] = ToGamma(c[0]); colour[i * 3 + 1] = ToGamma(c[1]); colour[i * 3 + 2] = ToGamma(c[2]);
		}
		D3DLOCKED_RECT l;
		CHECK(depthTex->LockRect(0, &l, NULL, 0));
		for (int y = 0; y < H; y++) memcpy((char*)l.pBits + y * l.Pitch, &depth[y * W], W * 4);
		depthTex->UnlockRect(0);
		CHECK(normalTex->LockRect(0, &l, NULL, 0));
		for (int y = 0; y < H; y++) D3DXFloat32To16Array((D3DXFLOAT16*)((char*)l.pBits + y * l.Pitch), &normals[y * W * 4], W * 4);
		normalTex->UnlockRect(0);
		CHECK(sceneTex->LockRect(0, &l, NULL, 0));
		for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
			const float* c = &colour[(y * W + x) * 3];
			auto b = [](float v) { return (DWORD)(min(1.0f, max(0.0f, v)) * 255.0f + 0.5f); };
			((DWORD*)((char*)l.pBits + y * l.Pitch))[x] = 0xFF000000 | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
		}
		sceneTex->UnlockRect(0);
		return 0;
	};

	ID3DXEffect* effect = nullptr;
	ID3DXBuffer* errors = nullptr;
	const D3DXMACRO barsMacros[] = { { "BLOOD_BARS", "1" }, { NULL, NULL } };
	if (FAILED(D3DXCreateEffectFromFileA(device, argv[1], bars ? barsMacros : NULL, NULL, 0, NULL, &effect, &errors))) {
		std::printf("effect failed: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
		return 1;
	}
	// The rivulets' streak texture (TESR_BloodStreaks, stage 7), from the tree the effect is in: <tree>\src\hlsl\... ->
	// <tree>\resource\Textures\Precipitations\water-streaks_n.dds.
	IDirect3DTexture9* streaksTex = nullptr;
	IDirect3DVolumeTexture9* noiseTex = nullptr;
	// The game's blood splatter decals (TESR_BloodSplatter, stage 9): BLOOD_SPLATTER, else the user's game folder; none: the
	// shader's drawn shapes (TESR_BloodFlowProbe.w 0).
	IDirect3DTexture9* splatterTex = nullptr;
	IDirect3DTexture9* smallSplatterTex = nullptr;
	D3DXCreateTextureFromFileA(device, getenv("BLOOD_SPLATTER") ? getenv("BLOOD_SPLATTER") : "Data\\Textures\\decals\\fxbloodsplatterdecals01.dds", &splatterTex);
	std::printf("splatter decals: %s\n", splatterTex ? "loaded" : "none (drawn shapes)");
	{
		std::string tree = argv[1];
		const size_t at = tree.find("src\\hlsl");
		tree = at == std::string::npos ? std::string() : tree.substr(0, at);
		const std::string streaks = tree + "resource\\Textures\\Effects\\BloodRuns.dds";	// UNOFFICIAL 2026-10-13: the blood runs (was the rain streaks)
		if (FAILED(D3DXCreateTextureFromFileA(device, streaks.c_str(), &streaksTex))) { std::printf("streak texture not found: %s\n", streaks.c_str()); return 1; }
		// The flow's distortion noise (TESR_NoiseVolume, stage 8), after the rewrite.
		const std::string noise = tree + "resource\\Textures\\Effects\\NoiseVolume.dds";
		if (FAILED(D3DXCreateVolumeTextureFromFileA(device, noise.c_str(), &noiseTex))) { std::printf("noise texture not found: %s\n", noise.c_str()); return 1; }
		// UNOFFICIAL 2026-10-13: the small splatters where drops land (TESR_BloodSpatterSmall, stage 10).
		D3DXCreateTextureFromFileA(device, (tree + "resource\\Textures\\Effects\\BloodSpatterSmall.dds").c_str(), &smallSplatterTex);
	}
	auto vec = [&](const char* name, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); effect->SetVector(name, &v); };
	auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
	// The defaults in VegasShaders.defaults.toml ([Shaders.BloodFlow.Main]).
	const float dripLength = 40.0f, maxFall = 200.0f;
	vec("TESR_ReciprocalResolution", 1.0f / W, 1.0f / H, (float)W / H, 0);
	vec("TESR_SunDirection", sun.x, sun.y, sun.z, 1);
	vec("TESR_SunColor", 1.0f, 0.92f, 0.8f, 1);
	vec("TESR_SunAmbient", 0.35f, 0.33f, 0.3f, 1);
	vec("TESR_BloodFlowFresh", lin(0.42f), lin(0.03f), lin(0.028f), 0.5f);
	vec("TESR_BloodFlowDry", lin(0.24f), lin(0.085f), lin(0.065f), 120.0f);
	vec("TESR_BloodFlowPool", 25.0f, dripLength, maxFall, 687.0f);
	vec("TESR_BloodFlowProbe", 1.0f / PathRows, 0.8f, (float)PathRows, splatterTex ? 1.0f : 0.0f);
	// UNOFFICIAL 2026-10-13: matched colour off here (no game decals), trails at their normal width; dried / fresh from the default colours.
	vec("TESR_BloodFlowMatch", 0.12f, 1.0f, 1.0f, 0.0f);
	vec("TESR_BloodFlowDryRatio", lin(0.24f) / lin(0.42f), lin(0.085f) / lin(0.03f), lin(0.065f) / lin(0.028f), 0.0f);
	vec("TESR_DepthConstants", 0, 0, 0, 1);
	vec("TESR_CameraData", NearZ, FarZ, 0, 0);
	vec("TESR_HorizonColor", 0.62f, 0.68f, 0.78f, 1);
	// 2026-10-16 (GroundFit): BLOOD_GROUND=0 the old hard cuts; BLOOD_UNSEEN=1 pools as if the ground round them was never found;
	// BLOOD_POOL_DZ moves the pools' centres up or down (the game puts them 1.5 units under the body's lowest bone, not on the ground).
	const bool groundFit = !getenv("BLOOD_GROUND") || atoi(getenv("BLOOD_GROUND")) != 0;
	const float poolDz = getenv("BLOOD_POOL_DZ") ? (float)atof(getenv("BLOOD_POOL_DZ")) : 0.0f;
	vec("TESR_BloodFlowGround", groundFit ? 1.0f : 0.0f, 3.0f, 0.0f, getenv("BLOOD_UNSEEN") && atoi(getenv("BLOOD_UNSEEN")) ? 1.0f : 0.0f);
	// 2026-10-27 the wet look (BloodFlow.cpp BloodFlowLook, defaults Sheen 0.45, SheenTint 0.6, MatchFloor 0.2); BLOOD_OLDLOOK=1 the look before.
	if (getenv("BLOOD_OLDLOOK") && atoi(getenv("BLOOD_OLDLOOK"))) vec("TESR_BloodFlowLook", 1.0f, 1.0f, 0.0f, 0.0f);
	else vec("TESR_BloodFlowLook", 0.45f, 0.25f, 0.6f, lin(0.2f));
	BloodFlowBoxes::SplatUp() = groundFit ? 1.2f + 2.4f * 1.0f : 1.0f;	// (SplatReach 1)
	BloodFlowBoxes::PoolUp() = groundFit ? 1.25f : 0.3f;

	const D3DVERTEXELEMENT9 elements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
		{ 0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
		{ 0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
		D3DDECL_END() };
	IDirect3DVertexDeclaration9* declaration;
	CHECK(device->CreateVertexDeclaration(elements, &declaration));
	static VolumetricSmokeQuads::Vertex vertices[ItemMax * 36], probeVertices[ItemMax * 6], boundsVertices[ItemMax * 6];
	IDirect3DSurface9 *image, *source, *scene, *pathSurface[2], *pathReadSurface, *backDepth = nullptr;
	imageTex->GetSurfaceLevel(0, &image);
	sourceTex->GetSurfaceLevel(0, &source);
	sceneTex->GetSurfaceLevel(0, &scene);
	pathRead->GetSurfaceLevel(0, &pathReadSurface);
	for (int i = 0; i < 2; i++) pathTex[i]->GetSurfaceLevel(0, &pathSurface[i]);
	device->GetDepthStencilSurface(&backDepth);

	// The items: the splats and two pools (on the flat floor; on the mound).
	std::vector<BloodFlowBoxes::Item> items;
	for (size_t i = 0; i < splats.size(); i++) {
		BloodFlowBoxes::Item it = {};
		it.Center = splats[i].at; it.Normal = splats[i].hint; D3DXVec3Normalize(&it.Normal, &it.Normal);
		it.Radius = splats[i].r; it.Seed = Hash(splats[i].at.x, splats[i].at.z + 1.0f); it.Fade = 1.0f; it.Pool = false; it.Slot = (int)i;
		// 2026-10-14: a decal layout as the game reports one (BloodFlow.cpp DecalLayout), so the shader reads the splatter's own
		// blood where the streams start: u across the splat's surface over its width (2 x radius), one of the four quarters,
		// width = height (ratio code 32).
		if (getenv("BLOOD_NO_LAYOUT") == nullptr) {
			D3DXVECTOR3 up(0, 0, 1), u;
			D3DXVec3Cross(&u, &up, &it.Normal);
			if (D3DXVec3Length(&u) < 0.1f) u = D3DXVECTOR3(1, 0, 0);
			D3DXVec3Normalize(&u, &u);
			it.DecalU = u / (2.0f * it.Radius);
			it.DecalCode = (int)(i % 4) + 20 * 32;
		}
		items.push_back(it);
	}
	if (TerrainMode) {	// two pools under bodies, on slopes
		BloodFlowBoxes::Item it = {};
		it.Normal = D3DXVECTOR3(0, 0, 1); it.Fade = 1; it.Pool = true;
		it.Center = D3DXVECTOR3(-80, 160, GroundZ(-80, 160) - 1.5f + poolDz); it.Radius = 30; it.Seed = 0.31f; it.Slot = 10;
		items.push_back(it);
		it.Center = D3DXVECTOR3(110, 90, GroundZ(110, 90) - 1.5f + poolDz); it.Radius = 26; it.Seed = 0.77f; it.Slot = 11;
		items.push_back(it);
	}
	else {
		BloodFlowBoxes::Item it = {};
		it.Center = D3DXVECTOR3(-45, 228, poolDz); it.Normal = D3DXVECTOR3(0, 0, 1); it.Radius = 30; it.Seed = 0.31f; it.Fade = 1; it.Pool = true; it.Slot = 10;
		items.push_back(it);
		it.Center = D3DXVECTOR3(-170, 140, RampZ(Ramps[1], -170, 140) - 1.5f + poolDz); it.Radius = 28; it.Seed = 0.77f; it.Slot = 11;
		// 2026-10-16 (GroundFit): BLOOD_POOL_STEEP=1 puts the second pool on the steep ramp (about 39 degrees) instead.
		if (getenv("BLOOD_POOL_STEEP") && atoi(getenv("BLOOD_POOL_STEEP"))) it.Center = D3DXVECTOR3(195, 175, RampZ(Ramps[0], 195, 175) - 1.5f + poolDz);
		items.push_back(it);
	}
	int passes = 0, pathIndex = 0;
	// GPU timing (timestamp queries round the probe pass and the boxes), when `timing` is set.
	bool timing = false;
	IDirect3DQuery9 *stamp[3] = {}, *disjoint = nullptr, *frequency = nullptr;
	for (int i = 0; i < 3; i++) device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &stamp[i]);
	device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint);
	device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency);
	double probeMs = 0.0, boxMs = 0.0;
	int written[ItemMax];
	for (int i = 0; i < ItemMax; i++) written[i] = -10;

	// One frame: the probe pass (paths), then the boxes, at `age`, from `cam` (as BloodFlowEffect::Render).
	auto frame = [&](const Camera& cam, float age, float debug, bool draw) -> int {
		vec("TESR_CameraPosition", cam.eye.x, cam.eye.y, cam.eye.z, 1);
		effect->SetMatrix("TESR_ViewTransform", &cam.view);
		effect->SetMatrix("TESR_InvViewTransform", &cam.invView);
		effect->SetMatrix("TESR_ProjectionTransform", &cam.proj);
		effect->SetMatrix("TESR_InvProjectionTransform", &cam.invProj);
		// The boxes are projected by the vertex shader (CutBox), cut to the bounds unless BLOOD_CUT=0.
		effect->SetMatrix("TESR_ViewProjectionTransform", &cam.viewProj);
		static const bool cut = !getenv("BLOOD_CUT") || atoi(getenv("BLOOD_CUT")) != 0;
		vec("TESR_BloodFlowShape", 0.0f, 0.0f, 0.0f, cut ? 1.0f : 0.0f);
		vec("TESR_BloodFlowInfo", 1.0f, debug, 300.0f, flowSpeed);
		vec("TESR_BloodFlowFlow", flowSpeed, viscosity, amount, age);	// the effect's clock: here the blood's age
		vec("TESR_BloodFlowSplat", 1.0f, 1.0f, 1.0f, 1.0f);	// 2026-10-14: the splats' own shapes (defaults)
		for (auto& it : items) { it.Age = age; it.Fresh = written[it.Slot] != passes - 1; }
		CHECK(device->UpdateSurface(scene, NULL, image, NULL));
		CHECK(device->StretchRect(image, NULL, source, NULL, D3DTEXF_NONE));
		CHECK(device->BeginScene());
		auto bind = [&](IDirect3DTexture9* paths) {
			// NVR binds each TESR_ texture straight to its sampler's register (EffectRecord::SetCT).
			device->SetTexture(0, depthTex); device->SetTexture(1, sourceTex); device->SetTexture(2, normalTex);
			device->SetTexture(3, shadowTex); device->SetTexture(4, viewModelTex); device->SetTexture(5, paths); device->SetTexture(6, boundsTex);
		device->SetTexture(7, streaksTex); device->SetTexture(8, noiseTex); device->SetTexture(9, splatterTex); device->SetTexture(10, smallSplatterTex);
		};
		const int probeTriangles = BloodFlowBoxes::BuildProbe(items.data(), (int)items.size(), dripLength, PathRows, BloodFlowBoxes::Streams, cam.eye, probeVertices);
		const int write = 1 - pathIndex;
		if (timing) { disjoint->Issue(D3DISSUE_BEGIN); stamp[0]->Issue(D3DISSUE_END); }
		CHECK(device->SetRenderTarget(0, pathSurface[write]));
		device->SetDepthStencilSurface(nullptr);
		effect->SetTechnique("BloodFlowProbe");
		UINT n = 0;
		CHECK(effect->Begin(&n, 0));
		CHECK(effect->BeginPass(0));
		bind(pathTex[pathIndex]);
		device->SetVertexDeclaration(declaration);
		CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, probeTriangles, probeVertices, sizeof(VolumetricSmokeQuads::Vertex)));
		effect->EndPass();
		effect->End();
		for (auto& it : items) { written[it.Slot] = passes; it.Fresh = false; }
		passes++;
		pathIndex = write;
		// The bounds pass, from the paths just written.
		const int boundsTriangles = BloodFlowBoxes::BuildProbe(items.data(), (int)items.size(), dripLength, ItemMax, 1, cam.eye, boundsVertices);
		CHECK(device->SetRenderTarget(0, boundsSurface));
		device->SetTexture(6, NULL);
		effect->SetTechnique("BloodFlowBounds");
		CHECK(effect->Begin(&n, 0));
		CHECK(effect->BeginPass(0));
		bind(pathTex[pathIndex]);
		device->SetTexture(6, NULL);
		CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, boundsTriangles, boundsVertices, sizeof(VolumetricSmokeQuads::Vertex)));
		effect->EndPass();
		effect->End();
		device->SetDepthStencilSurface(backDepth);
		if (timing) stamp[1]->Issue(D3DISSUE_END);
		if (draw) {
			float bounds[4];
			const int triangles = BloodFlowBoxes::Build(items.data(), (int)items.size(), dripLength, maxFall, cam.eye, D3DXVECTOR3(0, 0, 0), cam.viewProj, vertices, bounds);
			CHECK(device->SetRenderTarget(0, image));
			effect->SetTechnique("BloodFlow");
			CHECK(effect->Begin(&n, 0));
			CHECK(effect->BeginPass(0));
			bind(pathTex[pathIndex]);
			device->SetTexture(D3DVERTEXTEXTURESAMPLER0, boundsTex);
			device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			device->SetVertexDeclaration(declaration);
			if (triangles) CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, vertices, sizeof(VolumetricSmokeQuads::Vertex)));
			device->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);
			effect->EndPass();
			effect->End();
			// 2026-10-14: each splat's own impact shape (SplatPS; pools discard), as BloodFlow.cpp draws it after the streams.
			if (getenv("BLOOD_NO_SPLAT") == nullptr && effect->GetTechniqueByName("BloodSplat")) {
				effect->SetTechnique("BloodSplat");
				CHECK(effect->Begin(&n, 0));
				CHECK(effect->BeginPass(0));
				bind(pathTex[pathIndex]);
				device->SetVertexDeclaration(declaration);
				if (triangles) CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, vertices, sizeof(VolumetricSmokeQuads::Vertex)));
				effect->EndPass();
				effect->End();
			}
		}
		device->EndScene();
		if (timing) {
			stamp[2]->Issue(D3DISSUE_END);
			disjoint->Issue(D3DISSUE_END);
			frequency->Issue(D3DISSUE_END);
			UINT64 t[3] = {}, freq = 1;
			BOOL broken = TRUE;
			for (int i = 0; i < 3; i++) while (stamp[i]->GetData(&t[i], sizeof(UINT64), D3DGETDATA_FLUSH) == S_FALSE) {}
			while (disjoint->GetData(&broken, sizeof(BOOL), D3DGETDATA_FLUSH) == S_FALSE) {}
			while (frequency->GetData(&freq, sizeof(UINT64), D3DGETDATA_FLUSH) == S_FALSE) {}
			if (!broken) { probeMs += (double)(t[1] - t[0]) * 1000.0 / freq; boxMs += (double)(t[2] - t[1]) * 1000.0 / freq; }
		}
		return 0;
	};
	// The path texture's rows, read back.
	auto readPaths = [&](std::vector<float>& out) -> int {
		CHECK(device->GetRenderTargetData(pathSurface[pathIndex], pathReadSurface));
		D3DLOCKED_RECT l;
		CHECK(pathReadSurface->LockRect(&l, NULL, D3DLOCK_READONLY));
		out.resize(PathRows * BloodFlowBoxes::PathColumns * 4);
		for (int y = 0; y < PathRows; y++) memcpy(&out[y * BloodFlowBoxes::PathColumns * 4], (char*)l.pBits + y * l.Pitch, BloodFlowBoxes::PathColumns * 16);
		pathReadSurface->UnlockRect();
		return 0;
	};
	auto texel = [](const std::vector<float>& p, int col, int row) { return &p[(row * BloodFlowBoxes::PathColumns + col) * 4]; };
	auto save = [&](const std::string& name) -> int {
		const std::string path = outDir + "\\blood3_" + (close ? "close_" : "") + name + ".png";
		CHECK(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, image, NULL, NULL));
		std::printf("%s\n", path.c_str());
		return 0;
	};

	const Camera camA = TerrainMode ? MakeCamera(D3DXVECTOR3(0, -140, GroundZ(0, -140) + 150), 0.0f, 32.0f)
		: close ? MakeCamera(D3DXVECTOR3(40, 20, 120), 4.0f, 16.0f) : MakeCamera(D3DXVECTOR3(10, -150, 175), 0.0f, 20.0f);
	const Camera camB = close ? MakeCamera(D3DXVECTOR3(-20, 60, 100), -12.0f, 10.0f) : MakeCamera(D3DXVECTOR3(150, -80, 130), -16.0f, 14.0f);
	if (renderScene(camA)) return 1;

	// Settle the paths (kept after a few frames), then report what the probe found.
	for (int i = 0; i < 4; i++) if (frame(camA, 0.0f, 0, false)) return 1;
	std::vector<float> pathsA;
	if (readPaths(pathsA)) return 1;
	static const char* endNames[] = { "ran its length", "gathers on the flat", "drips off an edge" };
	for (const auto& it : items) {
		for (int k = 0; k < BloodFlowBoxes::Streams; k++) {
			const int row = it.Slot * BloodFlowBoxes::Streams + k;
			const float* h = texel(pathsA, 0, row);
			const float* m = texel(pathsA, 1, row);
			if (h[3] < -1.5f) { std::printf("item %d (pool) ground: seen %d frames, %d ring points, normal (%.2f %.2f %.2f) height %.1f\n", it.Slot, (int)h[0], (int)h[1], m[0], m[1], m[2], m[3]); continue; }
			if (h[3] < -0.5f) continue;
			const float* s = texel(pathsA, 2, row);
			const float* e = texel(pathsA, BloodFlowBoxes::PathColumns - 1, row);
			std::printf("item %d stream %d: seen %.0f frames, runs %.1f of %.1f, %s, fall %.1f, start (%.1f %.1f %.1f) end (%.1f %.1f %.1f) from centre\n", it.Slot, k, h[0], h[1], m[3],
				endNames[(int)(h[3] + 0.5f) % 3], h[2], s[0], s[1], s[2], e[0], e[1], e[2]);
		}
	}

	// 2026-10-16 (GroundFit): the cuts at 12 s: debug view 2 and the picture, cuts_<scene>_<BLOOD_TAG>[_debug].png, and the GPU cost.
	// BLOOD_CAM=near: low and close over the second pool (terrain) / the wall splats (room).
	if (cutsMode) {
		const char* tag = getenv("BLOOD_TAG") ? getenv("BLOOD_TAG") : "x";
		const bool nearCam = getenv("BLOOD_CAM") && !strcmp(getenv("BLOOD_CAM"), "near");
		const Camera cam = !nearCam ? camA : TerrainMode ? MakeCamera(D3DXVECTOR3(95, 20, GroundZ(95, 20) + 75), 12.0f, 38.0f)
			: MakeCamera(D3DXVECTOR3(-60, 170, 80), -15.0f, 8.0f);
		if (nearCam) { if (renderScene(cam)) return 1; for (int i = 0; i < ItemMax; i++) written[i] = -10; for (int i = 0; i < 4; i++) if (frame(cam, 0.0f, 0, false)) return 1; }
		for (int pass = 0; pass < 2; pass++) {
			BloodFlowBoxes::SplatUp() = groundFit || pass ? 1.2f + 2.4f * 1.0f : 1.0f;	// (debug view 2 shows what the old top cut)
			if (frame(cam, 12.0f, pass ? 2.0f : 0.0f, true)) return 1;
			char name[512];
			std::snprintf(name, sizeof(name), "%s\\cuts_%s%s_%s%s.png", outDir.c_str(), TerrainMode ? "terrain" : "room", nearCam ? "_near" : "", tag, pass ? "_debug" : "");
			CHECK(D3DXSaveSurfaceToFileA(name, D3DXIFF_PNG, image, NULL, NULL));
			std::printf("%s\n", name);
		}
		BloodFlowBoxes::SplatUp() = groundFit ? 1.2f + 2.4f * 1.0f : 1.0f;
		if (stamp[0] && disjoint && frequency) {
			for (int i = 0; i < 200; i++) if (frame(cam, 12.0f, 0, true)) return 1;
			timing = true; probeMs = boxMs = 0.0;
			for (int i = 0; i < 200; i++) if (frame(cam, 12.0f, 0, true)) return 1;
			std::printf("CUTS COST %s: probe + bounds %.4f ms, boxes (paint, shapes) %.4f ms\n", tag, probeMs / 200.0, boxMs / 200.0);
			timing = false;
		}
		return 0;
	}
	if (TerrainMode) {
		// The check: every ground splat on a slope steeper than about 5 degrees should send streams downhill.
		int sloped = 0, running = 0;
		for (const auto& it : items) {
			if (it.Pool) continue;
			const D3DXVECTOR3 n = GroundNormal(it.Center.x, it.Center.y);
			const float degrees = D3DXToDegree(acosf(n.z));
			float longest = 0.0f;
			for (int k = 0; k < BloodFlowBoxes::Streams; k++) {
				const float* h = texel(pathsA, 0, it.Slot * BloodFlowBoxes::Streams + k);
				if (h[3] > -0.5f) longest = max(longest, h[1]);
			}
			if (degrees > 5.0f) { sloped++; if (longest > 4.0f) running++; }
			std::printf("TERRAIN splat %d: ground slope %.1f degrees, longest stream %.1f units%s\n", it.Slot, degrees, longest,
				degrees > 5.0f ? (longest > 4.0f ? " (runs downhill: ok)" : " (DOES NOT RUN)") : " (about level: stays)");
		}
		std::printf("TERRAIN CHECK: %d of %d splats on slopes over 5 degrees run downhill: %s\n", running, sloped, running == sloped ? "PASS" : "FAIL");
		// The obstacle check: path points (each stream's 14) on a rock's upper half = blood running over it.
		int over = 0, nearRock = 0, total = 0;
		for (const auto& it : items) for (int k = 0; k < BloodFlowBoxes::Streams; k++) {
			const int row = it.Slot * BloodFlowBoxes::Streams + k;
			if (texel(pathsA, 0, row)[3] < -0.5f) continue;
			bool met = false;
			// (A stream that starts on a rock has not climbed it.)
			bool startsOnRock = false;
			for (const auto& r : Rocks) { const float* q = texel(pathsA, 2, row); const float dx = it.Center.x + q[0] - r[0], dy = it.Center.y + q[1] - r[1]; startsOnRock |= sqrtf(dx * dx + dy * dy) < r[2]; }
			if (startsOnRock) continue;
			for (int c = 2; c < BloodFlowBoxes::PathColumns; c++) {
				const float* q = texel(pathsA, c, row);
				const float x = it.Center.x + q[0], y = it.Center.y + q[1];
				for (const auto& r : Rocks) {
					const float d = sqrtf((x - r[0]) * (x - r[0]) + (y - r[1]) * (y - r[1]));
					if (d < r[2] * 0.55f) { over++; std::printf("  on a rock: item %d stream %d point %d at (%.1f %.1f), rock at (%.0f %.0f)\n", it.Slot, k, c, x, y, r[0], r[1]); }
					if (d < r[2] * 1.6f) met = true;
				}
				total++;
			}
			nearRock += met;
		}
		std::printf("OBSTACLE CHECK: %d streams came near a rock; %d of %d path points on a rock's top%s\n", nearRock, over, total,
			!WithRocks ? " (no rocks)" : over == 0 ? ": PASS" : ": FAIL");
		// The time-lapse: 0-20 s every 0.5 s, then the debug view at the same times.
		for (int pass = 0; pass < 2; pass++) for (int f = 0; f <= 40; f++) {
			if (frame(camA, f * 0.5f, (float)pass, true)) return 1;
			char name[512];
			std::snprintf(name, sizeof(name), "%s\\terrain_%s%03d.png", outDir.c_str(), pass ? "debug_" : "", f);
			CHECK(D3DXSaveSurfaceToFileA(name, D3DXIFF_PNG, image, NULL, NULL));
		}
		// Close up, low over the sloped splat at (60, 40), as a player looks at a body: the same 20 s.
		const Camera camClose = MakeCamera(D3DXVECTOR3(35, -25, GroundZ(35, -25) + 70), 18.0f, 42.0f);
		if (renderScene(camClose)) return 1;
		for (int i = 0; i < ItemMax; i++) written[i] = -10;
		for (int i = 0; i < 4; i++) if (frame(camClose, 0.0f, 0, false)) return 1;
		for (int f = 0; f <= 40; f++) {
			if (frame(camClose, f * 0.5f, 0.0f, true)) return 1;
			char name[512];
			std::snprintf(name, sizeof(name), "%s\\terrain_close_%03d.png", outDir.c_str(), f);
			CHECK(D3DXSaveSurfaceToFileA(name, D3DXIFF_PNG, image, NULL, NULL));
		}
		std::printf("time-lapse written: %s\\terrain_000.png ... terrain_040.png (and terrain_debug_*, terrain_close_*)\n", outDir.c_str());
		return running == sloped ? 0 : 1;
	}
	struct Shot { const char* name; float age, debug; };
	const Shot shots[] = { { "0_before", -1, 0 }, { "1_at_1s", 1, 0 }, { "2_at_3s", 3, 0 }, { "3_at_6s", 6, 0 }, { "4_at_12s", 12, 0 },
		{ "5_at_30s", 30, 0 }, { "6_dry_2min", 120, 0 }, { "7_dry_10min", 600, 0 }, { "8_debug_12s", 12, 1 } };
	for (const Shot& shot : shots) {
		if (shot.age < 0) CHECK(device->UpdateSurface(scene, NULL, image, NULL));
		else if (frame(camA, shot.age, shot.debug, true)) return 1;
		if (save(shot.name)) return 1;
	}
	// Drops falling off the table's edge: 24 frames 1/8 s apart, the part of the picture round the fall put side by side
	// (6 x 4 tiles, read left to right, top to bottom) in blood3_drops.png.
	{
		IDirect3DSurface9* montage;
		CHECK(device->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &montage, NULL));
		CHECK(device->ColorFill(montage, NULL, 0xFF000000));
		auto toScreen = [&](const D3DXVECTOR3& p) {
			const D3DXVECTOR3 rel = p - camA.eye;
			D3DXVECTOR4 c;
			const D3DXVECTOR4 p4(rel.x, rel.y, rel.z, 1.0f);
			D3DXVec4Transform(&c, &p4, &camA.viewProj);
			return D3DXVECTOR2((0.5f + 0.5f * c.x / c.w) * W, (0.5f - 0.5f * c.y / c.w) * H);
		};
		const D3DXVECTOR2 top = toScreen(D3DXVECTOR3(68, 150, 64)), bottom = toScreen(D3DXVECTOR3(68, 150, -2));
		const float tall = bottom.y - top.y + 60.0f, wide = tall * (W / 6.0f) / (H / 4.0f);
		RECT crop = { (LONG)(top.x - wide * 0.5f), (LONG)(top.y - 40.0f), (LONG)(top.x + wide * 0.5f), (LONG)(top.y - 40.0f + tall) };
		crop.left = max(crop.left, 0L); crop.top = max(crop.top, 0L); crop.right = min(crop.right, (LONG)W); crop.bottom = min(crop.bottom, (LONG)H);
		for (int f = 0; f < 24; f++) {
			const float age = 2.0f + f * 0.125f;
			if (frame(camA, age, 0, true)) return 1;
			RECT tile = { (LONG)((f % 6) * W / 6), (LONG)((f / 6) * H / 4), (LONG)((f % 6 + 1) * W / 6 - 2), (LONG)((f / 6 + 1) * H / 4 - 2) };
			CHECK(device->StretchRect(image, &crop, montage, &tile, D3DTEXF_LINEAR));
		}
		const std::string path = outDir + "\\blood3_" + (close ? "close_" : "") + "drops.png";
		CHECK(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, montage, NULL, NULL));
		std::printf("%s\n", path.c_str());
		montage->Release();
	}

	// Cost: the probe pass (paths kept: only the copy) and the boxes, at 12 s, averaged over 100 frames; then the probe
	// pass with every path worked out afresh each frame (its worst case: new items, or ones never seen whole).
	if (stamp[0] && disjoint && frequency) {
		for (int i = 0; i < 200; i++) if (frame(camA, 12.0f, 0, true)) return 1;		// warm up (the GPU's clocks)
		timing = true;
		for (int i = 0; i < 100; i++) if (frame(camA, 12.0f, 0, true)) return 1;
		std::printf("GPU cost at %dx%d, 12 s, %d items: probe + bounds passes (paths kept) %.3f ms, boxes %.3f ms\n", W, H, (int)items.size(), probeMs / 100.0, boxMs / 100.0);
		probeMs = boxMs = 0.0;
		for (int i = 0; i < 100; i++) { for (auto& it : items) written[it.Slot] = -10; if (frame(camA, 12.0f, 0, true)) return 1; }
		std::printf("GPU cost, every path worked out afresh each frame: probe pass %.3f ms, boxes %.3f ms\n", probeMs / 100.0, boxMs / 100.0);
		timing = false;
		for (int i = 0; i < 4; i++) if (frame(camA, 12.0f, 0, false)) return 1;
	}

	// Camera move: the paths found from A are kept and drawn from B; then the same from B alone (afresh), to compare.
	if (renderScene(camB)) return 1;
	if (frame(camB, 12.0f, 0, true)) return 1;
	if (save("9_moved_kept_12s")) return 1;
	std::vector<float> kept;
	if (readPaths(kept)) return 1;
	for (auto& it : items) written[it.Slot] = -10;	// forget: everything starts afresh from B
	for (int i = 0; i < 4; i++) if (frame(camB, 12.0f, 0, false)) return 1;
	if (frame(camB, 12.0f, 0, true)) return 1;
	if (save("9_moved_fresh_12s")) return 1;
	std::vector<float> freshB;
	if (readPaths(freshB)) return 1;
	for (const auto& it : items) for (int k = 0; k < BloodFlowBoxes::Streams; k++) {
		const int row = it.Slot * BloodFlowBoxes::Streams + k;
		if (texel(kept, 0, row)[3] < -0.5f || texel(freshB, 0, row)[3] < -0.5f) continue;
		float worst = 0.0f;
		for (int c = 2; c < BloodFlowBoxes::PathColumns; c++) {
			const float* a = texel(kept, c, row), * b = texel(freshB, c, row);
			const float d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
			worst = max(worst, d);
		}
		std::printf("camera move, item %d stream %d: kept path vs path found from the new camera alone differ by up to %.2f units (B sees it whole: %d)\n",
			it.Slot, k, worst, (int)texel(freshB, 0, row)[0]);
	}
	return 0;
}
