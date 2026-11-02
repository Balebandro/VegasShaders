// UNOFFICIAL 2026-10-29 (docs/next/ROOT-ASSUMPTIONS-PLAN.md, C: "measure the GTX 1070 loop cost first"): what a lamp costs a pixel on
// D3D9 (ps_3_0) three ways, on a full-screen pass at 1920x1080 into the scene's fp16 format:
//   unroll   the lamps in pixel constants, [unroll] (what the templates do now: ManyLightsDirect.hlsl, Pointlights.hlsl)
//   loop     the lamps in a float texture, a real [loop] to a count from a constant, two tex2Dlod per lamp (LightCommon's per-draw list)
//   cluster  as loop, but the count and the lamps come from a per-pixel cluster: one more fetch per pixel and one per lamp (froxels)
// Same lamp maths in all three (Karis windowed inverse square, Lambert + Blinn); the pixels are checked to match to 1e-3.
// Prints ms per pass (GPU timestamps, 40 passes after a warm-up) per lamp count. Picks the NVIDIA adapter when there is one.
// Usage: lamp_loop_cost.exe [adapter index] [--atlas 8192|4096|2048] [--counts 1,2,4,...]
//
// UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0d: the deferred lamp pass's cost before the shader is written): three more
// variants add the SHADOW ATLAS lookup per looped lamp, as Includes/PointShadowForward.hlsl PointShadowCube does it with POINT_SHADOW_ATLAS
// (normal offset, bias, the 4 + 1 tap blocker search on the lamp's octahedral tile, early out when nothing blocks, else a 6-tap PCF ring at
// the hardened spread; the tile (u, v, span) is a third table texel per lamp, as the plan's tile row):
//   loop+atlas lit    every tile holds 1 (nothing recorded): the lookup ends after the 5 search taps  (the cheap case)
//   loop+atlas busy   the tiles hold a checker of blockers: the search finds some, the ring runs too (11 taps a lamp)  (the expensive case)
//   cluster+atlas     the froxel list with the busy atlas: the pass's own cost structure (froxel fetch + per lamp 3 table fetches + 11 taps)
// The atlas is an R16F texture of the atlas's size (8192 by default = 128 MB, the game's; --atlas 4096 for a tighter card), live tiles of
// 1024 texels. The lit atlas variant must match the plain loop to 1e-3 (the early-out returns 1): a check of the lookup path.
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %s (0x%08lX) line %d\n", #x, (unsigned long)hr_, __LINE__); exit(1); } } while (0)

static const char* kCommon =
	"float3 Shade(float3 pos, float3 n, float3 v, float4 lp, float4 lc) {\n"
	"  float3 l = lp.xyz - pos; float d2 = dot(l, l); float r = lp.w;\n"
	"  float f = saturate(1.0f - (d2 * d2) / (r * r * r * r)); float att = f * f / (d2 * 1.0e-4f + 1.0f);\n"
	"  l *= rsqrt(max(d2, 1.0e-6f)); float3 h = normalize(l + v);\n"
	"  return lc.rgb * att * (saturate(dot(n, l)) + pow(saturate(dot(n, h)), 32.0f) * lc.a); }\n"
	"void Surface(float2 uv, out float3 pos, out float3 n, out float3 v) {\n"
	"  pos = float3(uv * 1500.0f, 40.0f * sin(uv.x * 30.0f) * cos(uv.y * 23.0f));\n"
	"  n = normalize(float3(cos(uv.x * 40.0f) * 0.3f, sin(uv.y * 37.0f) * 0.3f, 1.0f)); v = normalize(float3(0.3f, 0.2f, 1.0f)); }\n";

// The atlas lookup, after PointShadowForward.hlsl (POINT_SHADOW_ATLAS, Medium taps + the blocker search; contact hardening's narrowing).
static const char* kAtlas =
	"sampler2D A : register(s3);\n"
	"float2 Oct(float3 v) { float3 n = v / (abs(v.x) + abs(v.y) + abs(v.z));\n"
	"  float2 sgn = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);\n"
	"  float2 p = n.z < 0.0f ? (1.0f - abs(n.yx)) * sgn : n.xy; return p * 0.5f + 0.5f; }\n"
	"float Tap(float4 tile, float3 v) { return tex2Dlod(A, float4(tile.xy + Oct(v) * tile.z, 0.0f, 0.0f)).r; }\n"
	"float Shadow(float4 tile, float4 lp, float3 pos, float3 n) {\n"
	"  [branch] if (!(tile.z > 0.0f)) return 1.0f;\n"
	"  float3 toPixel = pos - lp.xyz; float len = length(toPixel); float invR = 1.0f / lp.w;\n"
	"  [branch] if (!(len * invR < 1.0f)) return 1.0f;\n"
	"  float3 p = toPixel + n * (0.02f * len); float d = length(p) * invR; float threshold = d - 0.02f * d;\n"
	"  float3 dir = p * float3(1.0f, 1.0f, -1.0f);\n"
	"  float major = max(abs(dir.x), max(abs(dir.y), abs(dir.z))); float s = 0.01f * major; float wide = 2.0f * s;\n"
	"  float4 search = float4(Tap(tile, dir + wide * float3(1, 1, 1)), Tap(tile, dir + wide * float3(-1, -1, 1)),\n"
	"                         Tap(tile, dir + wide * float3(1, -1, -1)), Tap(tile, dir + wide * float3(-1, 1, -1)));\n"
	"  float centre = Tap(tile, dir);\n"
	"  float4 isB = (float4)(search <= threshold) * (float4)(search > 0.0f); float cB = (float)(centre <= threshold) * (float)(centre > 0.0f);\n"
	"  float blockers = dot(isB, 1.0f) + cB;\n"
	"  [branch] if (!(blockers > 0.0f)) return 1.0f;\n"
	"  float blocker = (dot(search * isB, 1.0f) + centre * cB) / blockers; float t = 1.0f / max(blocker, 0.0001f) - 1.0f / d;\n"
	"  s = clamp(t * 0.5f * s * d, 0.25f * s, wide);\n"
	"  float4 a = float4(Tap(tile, dir + float3(s, 0, 0)), Tap(tile, dir + float3(-s, 0, 0)), Tap(tile, dir + float3(0, s, 0)), Tap(tile, dir + float3(0, -s, 0)));\n"
	"  float2 b = float2(Tap(tile, dir + float3(0, 0, s)), Tap(tile, dir + float3(0, 0, -s)));\n"
	"  float blocked = dot((float4)(a <= threshold) * (float4)(a > 0.0f), 1.0f) + dot((float2)(b <= threshold) * (float2)(b > 0.0f), 1.0f) + cB;\n"
	"  return 1.0f - blocked / 7.0f; }\n";

static const int kTableWidth = 256;	// 3 texels a lamp (position + reach, colour, tile), 64 lamps
static std::string TexelX(const char* lampExpr, int texel) { char b[128]; snprintf(b, sizeof(b), "((%s) * 3 + %d + 0.5f) / %d.0f", lampExpr, texel, kTableWidth); return b; }

static std::string UnrollPS(int lamps) {
	char head[128]; snprintf(head, sizeof(head), "#define N %d\n", lamps);
	return std::string(head) + "float4 L[128] : register(c0);\n" + kCommon +
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 { float3 p, n, v; Surface(uv, p, n, v); float3 c = 0.0f;\n"
		"  [unroll] for (int i = 0; i < N; i++) c += Shade(p, n, v, L[i * 2], L[i * 2 + 1]);\n"
		"  return float4(c, 1.0f); }\n";
}
// (Measured 2026-10-29: a [loop] over pixel constants indexed by the loop counter does not compile for ps_3_0 -- D3DX copies the array
// into temp registers, X4505 "maximum temp register index exceeded" -- so a lamp table must come from a texture.)
static std::string LoopPS(bool atlas) {
	return std::string("sampler2D T : register(s0); float4 K : register(c0);\n") + kCommon + (atlas ? kAtlas : "") +
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 { float3 p, n, v; Surface(uv, p, n, v); float3 c = 0.0f; int count = (int)K.x;\n"
		"  [loop] for (int i = 0; i < 64; i++) { if (i >= count) break;\n"
		"    float4 lp = tex2Dlod(T, float4(" + TexelX("i", 0) + ", 0.5f, 0, 0)); float4 lc = tex2Dlod(T, float4(" + TexelX("i", 1) + ", 0.5f, 0, 0));\n" +
		(atlas ? "    float4 tile = tex2Dlod(T, float4(" + TexelX("i", 2) + ", 0.5f, 0, 0)); c += Shade(p, n, v, lp, lc) * Shadow(tile, lp, p, n); }\n"
			   : "    c += Shade(p, n, v, lp, lc); }\n") +
		"  return float4(c, 1.0f); }\n";
}
// Cluster texture C (16 x 9, point): x = count, y = first index; index texture I (1024 x 1): x = lamp number.
static std::string ClusterPS(bool atlas) {
	return std::string("sampler2D T : register(s0); sampler2D C : register(s1); sampler2D I : register(s2);\n") + kCommon + (atlas ? kAtlas : "") +
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 { float3 p, n, v; Surface(uv, p, n, v); float3 c = 0.0f;\n"
		"  float4 cl = tex2Dlod(C, float4(uv, 0, 0)); int count = (int)cl.x;\n"
		"  [loop] for (int i = 0; i < 64; i++) { if (i >= count) break;\n"
		"    float k = tex2Dlod(I, float4((cl.y + i + 0.5f) / 1024.0f, 0.5f, 0, 0)).x;\n"
		"    float4 lp = tex2Dlod(T, float4(" + TexelX("k", 0) + ", 0.5f, 0, 0)); float4 lc = tex2Dlod(T, float4(" + TexelX("k", 1) + ", 0.5f, 0, 0));\n" +
		(atlas ? "    float4 tile = tex2Dlod(T, float4(" + TexelX("k", 2) + ", 0.5f, 0, 0)); c += Shade(p, n, v, lp, lc) * Shadow(tile, lp, p, n); }\n"
			   : "    c += Shade(p, n, v, lp, lc); }\n") +
		"  return float4(c, 1.0f); }\n";
}

static IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const std::string& src, int* slots) {
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCompileShader(src.c_str(), (UINT)src.size(), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL))) {
		printf("compile failed: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
		exit(1);
	}
	IDirect3DPixelShader9* ps = nullptr;
	CHECK(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps));
	if (slots) {	// instruction slots, from the disassembly's last comment line ("// approximately N instruction slots used")
		ID3DXBuffer* dis = nullptr; *slots = -1;
		if (SUCCEEDED(D3DXDisassembleShader((const DWORD*)code->GetBufferPointer(), FALSE, NULL, &dis)) && dis) {
			const char* t = (const char*)dis->GetBufferPointer();
			const char* a = strstr(t, "approximately ");
			if (a) *slots = atoi(a + 14);
			dis->Release();
		}
	}
	code->Release(); if (errors) errors->Release();
	return ps;
}

// R16F atlas: 1 everywhere (nothing recorded), or a checker of blockers (stored distance / radius 0.3 on half the texels of every tile).
static void FillAtlas(IDirect3DTexture9* atlas, UINT size, bool busy) {
	D3DLOCKED_RECT r; CHECK(atlas->LockRect(0, &r, NULL, D3DLOCK_DISCARD));
	const unsigned short one = 0x3C00, blocker = 0x34CD;	// half floats 1.0 and ~0.3
	for (UINT y = 0; y < size; y++) {
		unsigned short* row = (unsigned short*)((char*)r.pBits + y * r.Pitch);
		for (UINT x = 0; x < size; x++) row[x] = busy && (((x >> 3) + (y >> 3)) & 1) ? blocker : one;
	}
	atlas->UnlockRect(0);
}

int main(int argc, char** argv) {
	UINT atlasSize = 8192; int adapterArg = -1; std::vector<int> counts = { 1, 2, 4, 6, 8, 12, 16, 24, 32 };
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--atlas") && i + 1 < argc) atlasSize = (UINT)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--counts") && i + 1 < argc) { counts.clear(); for (char* t = strtok(argv[++i], ","); t; t = strtok(NULL, ",")) counts.push_back(atoi(t)); }
		else adapterArg = atoi(argv[i]);
	}
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) { printf("no D3D9\n"); return 1; }
	UINT adapter = D3DADAPTER_DEFAULT;
	for (UINT a = 0; a < d3d->GetAdapterCount(); a++) {
		D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(a, 0, &id);
		printf("adapter %u: %s\n", a, id.Description);
		if (adapterArg < 0 && strstr(id.Description, "NVIDIA")) adapter = a;
	}
	if (adapterArg >= 0) adapter = (UINT)adapterArg;
	D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(adapter, 0, &id);
	printf("using %u: %s\n", adapter, id.Description);

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "lamploop"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("lamploop", "lamploop", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	IDirect3DDevice9* d = nullptr;
	CHECK(d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d));

	const UINT W = 1920, H = 1080;
	IDirect3DTexture9* target = nullptr; IDirect3DSurface9* targetSurface = nullptr;
	CHECK(d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &target, NULL));
	CHECK(target->GetSurfaceLevel(0, &targetSurface));
	IDirect3DSurface9* readTarget = nullptr; IDirect3DSurface9* readCopy = nullptr;
	CHECK(d->CreateRenderTarget(64, 36, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &readTarget, NULL));
	CHECK(d->CreateOffscreenPlainSurface(64, 36, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &readCopy, NULL));

	// The atlas (dynamic: lockable in the default pool), 8192 by default, smaller when the card refuses it.
	IDirect3DTexture9* atlas = nullptr;
	while (atlasSize >= 2048 && FAILED(d->CreateTexture(atlasSize, atlasSize, 1, D3DUSAGE_DYNAMIC, D3DFMT_R16F, D3DPOOL_DEFAULT, &atlas, NULL))) { printf("atlas %u refused\n", atlasSize); atlasSize /= 2; }
	if (!atlas) { printf("no atlas texture\n"); return 1; }
	printf("atlas %ux%u R16F (%.0f MB), tiles of 1024 texels (%d live tiles)\n", atlasSize, atlasSize, atlasSize * (double)atlasSize * 2.0 / 1048576.0, (int)((atlasSize / 1024) * (atlasSize / 1024)));
	const float tileSpan = 1024.0f / atlasSize;
	const int tilesAcross = (int)(atlasSize / 1024);

	// 64 lamps scattered over the surface, radius 300-700, three float4 each (position + radius, colour + specular, tile u v span).
	std::vector<float> lamps(64 * 12);
	for (int i = 0; i < 64; i++) {
		float* l = &lamps[i * 12];
		l[0] = 100.0f + fmodf(i * 397.0f, 1300.0f); l[1] = 100.0f + fmodf(i * 211.0f, 1300.0f); l[2] = 60.0f + (i % 5) * 20.0f; l[3] = 300.0f + (i % 9) * 50.0f;
		l[4] = 0.2f + (i % 3) * 0.1f; l[5] = 0.2f + (i % 4) * 0.08f; l[6] = 0.15f + (i % 5) * 0.05f; l[7] = 0.5f;
		const int tile = i % (tilesAcross * tilesAcross);
		l[8] = (tile % tilesAcross) * tileSpan; l[9] = (tile / tilesAcross) * tileSpan; l[10] = tileSpan; l[11] = 0.0f;
	}
	std::vector<float> lampsUnroll(64 * 8);	// the unrolled shader's constants: two float4 a lamp
	for (int i = 0; i < 64; i++) memcpy(&lampsUnroll[i * 8], &lamps[i * 12], 8 * sizeof(float));
	IDirect3DTexture9* lampTex = nullptr;
	CHECK(d->CreateTexture(kTableWidth, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &lampTex, NULL));
	{ D3DLOCKED_RECT r; CHECK(lampTex->LockRect(0, &r, NULL, 0)); memset(r.pBits, 0, kTableWidth * 16); memcpy(r.pBits, lamps.data(), lamps.size() * 4); lampTex->UnlockRect(0); }
	IDirect3DTexture9* clusterTex = nullptr; IDirect3DTexture9* indexTex = nullptr;
	CHECK(d->CreateTexture(16, 9, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &clusterTex, NULL));
	CHECK(d->CreateTexture(1024, 1, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &indexTex, NULL));
	{ D3DLOCKED_RECT r; CHECK(indexTex->LockRect(0, &r, NULL, 0)); float* x = (float*)r.pBits; for (int i = 0; i < 1024; i++) x[i] = (float)(i % 64); indexTex->UnlockRect(0); }

	struct V { float x, y, z, w, u, v; };
	const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { W - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, H - 0.5f, 0, 1, 0, 1 }, { W - 0.5f, H - 0.5f, 0, 1, 1, 1 } };
	d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING, FALSE);
	for (DWORD s = 0; s < 4; s++) {
		d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	IDirect3DQuery9 *q0 = nullptr, *q1 = nullptr, *freq = nullptr, *disjoint = nullptr;
	CHECK(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q0)); CHECK(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q1));
	CHECK(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq)); CHECK(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint));

	auto time = [&](IDirect3DPixelShader9* ps, int passes) -> double {
		d->SetRenderTarget(0, targetSurface);
		d->SetPixelShader(ps);
		d->BeginScene();
		for (int i = 0; i < 4; i++) d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));	// warm-up
		disjoint->Issue(D3DISSUE_BEGIN); freq->Issue(D3DISSUE_END); q0->Issue(D3DISSUE_END);
		for (int i = 0; i < passes; i++) d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		q1->Issue(D3DISSUE_END); disjoint->Issue(D3DISSUE_END);
		d->EndScene();
		UINT64 t0 = 0, t1 = 0, f = 0; BOOL dj = TRUE;
		while (q1->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (q0->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (freq->GetData(&f, sizeof(f), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (disjoint->GetData(&dj, sizeof(dj), D3DGETDATA_FLUSH) == S_FALSE) {}
		if (dj || !f) return -1.0;
		return (double)(t1 - t0) / (double)f * 1000.0 / passes;
	};
	auto readMean = [&](float out[3]) {
		d->StretchRect(targetSurface, NULL, readTarget, NULL, D3DTEXF_POINT);
		d->GetRenderTargetData(readTarget, readCopy);
		D3DLOCKED_RECT r; readCopy->LockRect(&r, NULL, D3DLOCK_READONLY);
		double s[3] = {};
		for (int y = 0; y < 36; y++) { const float* row = (const float*)((const char*)r.pBits + y * r.Pitch); for (int x = 0; x < 64; x++) for (int c = 0; c < 3; c++) s[c] += row[x * 4 + c]; }
		readCopy->UnlockRect();
		for (int c = 0; c < 3; c++) out[c] = (float)(s[c] / (64 * 36));
	};
	auto setCluster = [&](int n) {
		D3DLOCKED_RECT r; CHECK(clusterTex->LockRect(0, &r, NULL, 0));
		for (int y = 0; y < 9; y++) { float* row = (float*)((char*)r.pBits + y * r.Pitch); for (int x = 0; x < 16; x++) { row[x * 4] = (float)n; row[x * 4 + 1] = 0; row[x * 4 + 2] = 0; row[x * 4 + 3] = 0; } }
		clusterTex->UnlockRect(0);
	};

	int loopSlots = 0, clusterSlots = 0, loopAtlasSlots = 0, clusterAtlasSlots = 0;
	IDirect3DPixelShader9* loop = Compile(d, LoopPS(false), &loopSlots);
	IDirect3DPixelShader9* cluster = Compile(d, ClusterPS(false), &clusterSlots);
	IDirect3DPixelShader9* loopAtlas = Compile(d, LoopPS(true), &loopAtlasSlots);
	IDirect3DPixelShader9* clusterAtlas = Compile(d, ClusterPS(true), &clusterAtlasSlots);
	printf("instruction slots: loop %d, cluster %d, loop+atlas %d, cluster+atlas %d (any lamp count)\n", loopSlots, clusterSlots, loopAtlasSlots, clusterAtlasSlots);
	printf("%6s %10s %6s %10s %10s %10s | %12s %12s %14s | %s\n", "lamps", "unroll ms", "slots", "loop ms", "cluster ms", "loop/unr", "loop+atl lit", "loop+atl busy", "cluster+atl busy", "match (unr=loop=cluster=atlas lit)");
	for (int n : counts) {
		int slots = 0;
		IDirect3DPixelShader9* unroll = Compile(d, UnrollPS(n), &slots);
		double tu = 1e9, tl = 1e9, tc = 1e9, tal = 1e9, tab = 1e9, tcb = 1e9;
		float mu[3], ml[3], mc[3], mal[3];
		for (int rep = 0; rep < 5; rep++) {	// best of five: the GPU is shared (bench slots, other sessions)
			d->SetPixelShaderConstantF(0, lampsUnroll.data(), n * 2);
			double t = time(unroll, 40); if (t > 0 && t < tu) tu = t;
			if (rep == 0) readMean(mu);
			const float k[4] = { (float)n, 0, 0, 0 };
			d->SetPixelShaderConstantF(0, k, 1);
			d->SetTexture(0, lampTex);
			t = time(loop, 40); if (t > 0 && t < tl) tl = t;
			if (rep == 0) readMean(ml);
			setCluster(n);
			d->SetTexture(1, clusterTex); d->SetTexture(2, indexTex);
			t = time(cluster, 40); if (t > 0 && t < tc) tc = t;
			if (rep == 0) readMean(mc);
			// the atlas variants: lit (every tile 1) with the loop, busy (checker) with the loop and the cluster
			FillAtlas(atlas, atlasSize, false);
			d->SetTexture(3, atlas);
			d->SetPixelShaderConstantF(0, k, 1);
			t = time(loopAtlas, 40); if (t > 0 && t < tal) tal = t;
			if (rep == 0) readMean(mal);
			FillAtlas(atlas, atlasSize, true);
			t = time(loopAtlas, 40); if (t > 0 && t < tab) tab = t;
			t = time(clusterAtlas, 40); if (t > 0 && t < tcb) tcb = t;
		}
		const float e1 = fabsf(mu[0] - ml[0]) + fabsf(mu[1] - ml[1]) + fabsf(mu[2] - ml[2]);
		const float e2 = fabsf(mu[0] - mc[0]) + fabsf(mu[1] - mc[1]) + fabsf(mu[2] - mc[2]);
		const float e3 = fabsf(mu[0] - mal[0]) + fabsf(mu[1] - mal[1]) + fabsf(mu[2] - mal[2]);
		printf("%6d %10.3f %6d %10.3f %10.3f %10.2f | %12.3f %12.3f %14.3f | %s\n", n, tu, slots, tl, tc, tl / tu, tal, tab, tcb,
			(e1 < 1e-3f && e2 < 1e-3f && e3 < 1e-3f) ? "yes" : "NO");
		fflush(stdout);
		unroll->Release();
	}
	return 0;
}
