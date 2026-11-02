// Offline look check of the volumetric clouds (Effects\VolumetricClouds.fx.hlsl), without the game: renders the three
// techniques the way VolumetricCloudsEffect does (march, temporal, composite) over a plain sky gradient for a few views,
// sun positions and weathers, and saves each as a PNG. Run from src/hlsl/NewVegas/Effects.
// Usage: build\cloud-preview\cloud_preview.exe <Textures\Effects folder> <output folder> [frames] [width height]
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <cstring>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* what) {
	if (FAILED(hr)) { char text[256]; sprintf_s(text, "%s failed: %08lx", what, hr); throw std::runtime_error(text); }
}

struct View {
	const char* name;
	float yaw, pitch;          // degrees: where the camera looks
	float sunYaw, sunPitch;    // degrees: where the light comes from
	float day;                 // 0 night .. 1 day
	float coverage, rain, typeBias;
	float sunR, sunG, sunB;    // light colour (gamma)
	float skyR, skyG, skyB;    // sky top (gamma)
	float horR, horG, horB;    // horizon (gamma)
};

static D3DXVECTOR3 Direction(float yawDeg, float pitchDeg) {
	const float y = D3DXToRadian(yawDeg), p = D3DXToRadian(pitchDeg);
	return D3DXVECTOR3(cosf(y) * cosf(p), sinf(y) * cosf(p), sinf(p));
}

int main(int argc, char** argv) {
	if (argc < 3) { std::puts("usage: cloud_preview <Textures\\Effects folder> <output folder> [frames] [width height]"); return 2; }
	const std::string textures = argv[1], out = argv[2];
	const int frames = argc > 3 ? atoi(argv[3]) : 16;
	const UINT W = argc > 5 ? (UINT)atoi(argv[4]) : 640, H = argc > 5 ? (UINT)atoi(argv[5]) : 360;
	const UINT Div = getenv("CLOUD_DIV") ? (UINT)atoi(getenv("CLOUD_DIV")) : 4;
	const UINT LW = (W + Div - 1) / Div, LH = (H + Div - 1) / Div, BW = (LW + 1) / 2, BH = (LH + 1) / 2;
	const float farZ = 283840.0f;
	try {
		HWND window = CreateWindowExA(0, "STATIC", "clouds", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
		ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
		D3DDISPLAYMODE display = {}; d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display);
		D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
		pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = display.Format;
		ComPtr<IDirect3DDevice9> device;
		Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device), "device");

		ComPtr<ID3DXEffect> effect; ComPtr<ID3DXBuffer> errors;
		// UNOFFICIAL 2026-10-27: CLOUD_FX picks another effect file (a test copy)
		// (a .fxo: an effect compiled beforehand, fxc /T fx_2_0: seconds instead of the D3DX compiler's minutes for long loops)
		const char* fxFile = getenv("CLOUD_FX") ? getenv("CLOUD_FX") : "VolumetricClouds.fx.hlsl";
		HRESULT hr;
		const size_t fxLength = strlen(fxFile);
		if (fxLength > 4 && _stricmp(fxFile + fxLength - 4, ".fxo") == 0) {
			FILE* f = nullptr;
			if (fopen_s(&f, fxFile, "rb") || !f) throw std::runtime_error("cannot open the .fxo");
			std::string bytes;
			char chunk[65536];
			for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0; ) bytes.append(chunk, n);
			fclose(f);
			hr = D3DXCreateEffect(device.Get(), bytes.data(), (UINT)bytes.size(), NULL, NULL, 0, NULL, &effect, &errors);
		}
		else hr = D3DXCreateEffectFromFileA(device.Get(), fxFile, NULL, NULL, 0, NULL, &effect, &errors);
		if (errors) std::puts((const char*)errors->GetBufferPointer());
		Check(hr, "effect");

		ComPtr<IDirect3DVolumeTexture9> shape, detail;
		ComPtr<IDirect3DTexture9> weather, blue;
		ComPtr<IDirect3DVolumeTexture9> noiseVolume;
		Check(D3DXCreateVolumeTextureFromFileA(device.Get(), (textures + "\\NoiseVolume.dds").c_str(), &noiseVolume), "NoiseVolume.dds");
		Check(D3DXCreateVolumeTextureFromFileA(device.Get(), (textures + "\\CloudShape.dds").c_str(), &shape), "CloudShape.dds");
		Check(D3DXCreateVolumeTextureFromFileA(device.Get(), (textures + "\\CloudDetail.dds").c_str(), &detail), "CloudDetail.dds");
		Check(D3DXCreateTextureFromFileA(device.Get(), (textures + "\\CloudWeather.dds").c_str(), &weather), "CloudWeather.dds");
		Check(D3DXCreateTextureFromFileA(device.Get(), (textures + "\\bluenoise256.dds").c_str(), &blue), "bluenoise256.dds");

		auto target = [&](UINT w, UINT h, D3DFORMAT f, ComPtr<IDirect3DTexture9>& t, ComPtr<IDirect3DSurface9>& s) {
			Check(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t, NULL), "target");
			Check(t->GetSurfaceLevel(0, &s), "surface");
		};
		ComPtr<IDirect3DTexture9> current, history[2], final;
		ComPtr<IDirect3DSurface9> currentS, historyS[2], finalS;
		target(BW, BH, D3DFMT_A16B16G16R16F, current, currentS);
		target(LW, LH, D3DFMT_A16B16G16R16F, history[0], historyS[0]);
		target(LW, LH, D3DFMT_A16B16G16R16F, history[1], historyS[1]);
		target(W, H, D3DFMT_A8R8G8B8, final, finalS);
		ComPtr<IDirect3DTexture9> scene, depth;
		Check(device->CreateTexture(W, H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &scene, NULL), "scene");
		Check(device->CreateTexture(W, H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &depth, NULL), "depth");

		struct Vertex { float x, y, z, u, v; };
		auto quad = [&](UINT w, UINT h) {
			const float ox = 0.5f / w, oy = 0.5f / h;
			Vertex v[] = { {-1, 1, 1, ox, oy}, {-1, -1, 1, ox, 1 + oy}, {1, 1, 1, 1 + ox, oy}, {1, -1, 1, 1 + ox, 1 + oy} };
			Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex)), "draw");
		};
		auto vec = [&](const char* name, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); effect->SetVector(name, &v); };
		auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
		auto gam = [](float c) { c = c < 0 ? 0 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; };

		const View views[] = {
			{ "1-day-toward-sun",     0, 12,   10, 25, 1.0f, 0.45f, 0.0f, 0.0f, 1.0f, 0.95f, 0.85f, 0.30f, 0.50f, 0.85f, 0.70f, 0.80f, 0.92f },
			{ "2-day-sun-behind",   180, 12,   10, 45, 1.0f, 0.45f, 0.0f, 0.0f, 1.0f, 0.95f, 0.85f, 0.30f, 0.50f, 0.85f, 0.70f, 0.80f, 0.92f },
			{ "3-day-looking-up",    90, 55,   10, 45, 1.0f, 0.45f, 0.0f, 0.0f, 1.0f, 0.95f, 0.85f, 0.30f, 0.50f, 0.85f, 0.70f, 0.80f, 0.92f },
			{ "4-scattered",         90, 10,   10, 60, 1.0f, 0.25f, 0.0f, 0.3f, 1.0f, 0.97f, 0.90f, 0.28f, 0.48f, 0.85f, 0.70f, 0.80f, 0.92f },
			{ "5-overcast-rain",     90, 10,   10, 45, 1.0f, 0.95f, 1.0f, -0.3f, 0.75f, 0.75f, 0.75f, 0.45f, 0.48f, 0.52f, 0.58f, 0.60f, 0.62f },
			{ "6-sunset",             0,  8,    5,  4, 0.5f, 0.55f, 0.0f, 0.0f, 1.0f, 0.55f, 0.25f, 0.25f, 0.30f, 0.55f, 0.95f, 0.60f, 0.40f },
			{ "8-cloudy-up",         90, 35,   10, 40, 0.8f, 0.7f, 0.0f, 0.0f, 0.85f, 0.85f, 0.80f, 0.55f, 0.60f, 0.55f, 0.65f, 0.68f, 0.62f },
			{ "9-cloudy-horizon",    90, 10,   10, 40, 0.8f, 0.7f, 0.0f, 0.0f, 0.85f, 0.85f, 0.80f, 0.55f, 0.60f, 0.55f, 0.65f, 0.68f, 0.62f },
			// UNOFFICIAL 2026-10-20 Mojave fair-weather cumulus views (the user's reference photos): low, wide looks across the desert
			{ "m1-cumulus-across",   60,  6,   10, 40, 1.0f, 0.40f, 0.0f, 0.0f, 1.0f, 0.96f, 0.88f, 0.25f, 0.45f, 0.85f, 0.62f, 0.74f, 0.90f },
			{ "m2-cumulus-up",       60, 22,   10, 50, 1.0f, 0.45f, 0.0f, 0.2f, 1.0f, 0.96f, 0.88f, 0.25f, 0.45f, 0.85f, 0.62f, 0.74f, 0.90f },
			{ "m3-storm-build",     240,  8,   10, 35, 1.0f, 0.60f, 0.3f, 0.4f, 1.0f, 0.95f, 0.85f, 0.20f, 0.38f, 0.80f, 0.60f, 0.70f, 0.86f },
			{ "m4-low-sun",          30,  5,   10, 12, 0.9f, 0.55f, 0.0f, 0.0f, 1.0f, 0.85f, 0.65f, 0.30f, 0.45f, 0.75f, 0.80f, 0.75f, 0.70f },
			// UNOFFICIAL 2026-10-25 Towering: storms far off, a low warm sun behind the camera (the user's cumulonimbus photos)
			{ "m5-storm-far",       210,  4,   30,  9, 0.9f, 0.50f, 0.0f, 0.3f, 1.0f, 0.82f, 0.62f, 0.25f, 0.42f, 0.72f, 0.70f, 0.72f, 0.74f },
			{ "m6-storm-wide",      210, 14,   30,  9, 0.9f, 0.50f, 0.0f, 0.3f, 1.0f, 0.82f, 0.62f, 0.25f, 0.42f, 0.72f, 0.70f, 0.72f, 0.74f },
			// UNOFFICIAL 2026-10-27 (the user's grainy storm screenshot): the same storms at midday, a high sun to the side
			{ "m7-storm-noon",      210, 14,  120, 60, 1.0f, 0.50f, 0.0f, 0.3f, 1.0f, 0.97f, 0.90f, 0.28f, 0.48f, 0.85f, 0.70f, 0.80f, 0.92f },
			// (and close up: with CLOUD_CAMX=18929 CLOUD_CAMY=17870 the storm of cell 0, 0 is 5 km ahead, the sun high to the right)
			{ "m8-storm-near",       45, 16,  170, 55, 1.0f, 0.50f, 0.0f, 0.3f, 1.0f, 0.97f, 0.90f, 0.28f, 0.48f, 0.85f, 0.70f, 0.80f, 0.92f },
			{ "7-night",             90, 20,   10, 50, 0.0f, 0.5f, 0.0f, 0.0f, 0.25f, 0.30f, 0.40f, 0.03f, 0.05f, 0.10f, 0.06f, 0.08f, 0.14f },
		};
		const char* only = getenv("CLOUD_VIEWS");
		for (const View& v : views) {
			if (only) {   // space-separated name prefixes
				bool wanted = false;
				for (const char* t = only; *t; ) {
					while (*t == ' ') t++;
					size_t n = strcspn(t, " ");
					if (n && strncmp(v.name, t, n) == 0) wanted = true;
					t += n;
				}
				if (!wanted) continue;
			}
			// the scene: a sky gradient with a band of distant ground across the bottom (not sky in the depth buffer)
			// UNOFFICIAL 2026-10-25: CLOUD_YAW (added to the view's yaw), CLOUD_PITCH (replaces its pitch) and CLOUD_TAG (file name suffix) for sweeps
			const float yawAdd = getenv("CLOUD_YAW") ? (float)atof(getenv("CLOUD_YAW")) : 0.0f;
			const float pitch = getenv("CLOUD_PITCH") ? (float)atof(getenv("CLOUD_PITCH")) : v.pitch;
			D3DXVECTOR3 forward = Direction(v.yaw + yawAdd, pitch);
			D3DXVECTOR3 eye(0, 0, 0), up(0, 0, 1), at = forward;
			D3DXMATRIX view, proj, viewProj;
			D3DXMatrixLookAtLH(&view, &eye, &at, &up);
			D3DXMatrixPerspectiveFovLH(&proj, D3DXToRadian(70.0f), float(W) / H, 5.0f, farZ);
			viewProj = view * proj;
			D3DLOCKED_RECT s, d;
			scene->LockRect(0, &s, NULL, 0); depth->LockRect(0, &d, NULL, 0);
			D3DXMATRIX inverse; D3DXMatrixInverse(&inverse, NULL, &viewProj);
			for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
				D3DXVECTOR3 ndc((x + 0.5f) / W * 2 - 1, 1 - (y + 0.5f) / H * 2, 1.0f), world;
				D3DXVec3TransformCoord(&world, &ndc, &inverse);
				D3DXVec3Normalize(&world, &world);
				const float elevation = world.z;
				const float t = powf(1.0f - (elevation > 0 ? elevation : 0), 3.0f);
				float* sp = (float*)((char*)s.pBits + y * s.Pitch) + x * 4;
				float* dp = (float*)((char*)d.pBits + y * d.Pitch) + x * 4;
				const bool ground = elevation < -0.02f;
				sp[0] = ground ? 0.35f * v.day + 0.03f : v.skyR + (v.horR - v.skyR) * t;
				sp[1] = ground ? 0.30f * v.day + 0.03f : v.skyG + (v.horG - v.skyG) * t;
				sp[2] = ground ? 0.25f * v.day + 0.03f : v.skyB + (v.horB - v.skyB) * t;
				sp[3] = 1;
				dp[0] = ground ? 0.3f : 1.0f; dp[1] = dp[2] = 0; dp[3] = 1;
			}
			scene->UnlockRect(0); depth->UnlockRect(0);

			D3DXVECTOR3 sun = Direction(v.sunYaw, v.sunPitch);
			vec("TESR_ReciprocalResolution", 1.0f / W, 1.0f / H, 0, 0);
			vec("TESR_RenderScale", 1, 1, 1, 1);   // UNOFFICIAL 2026-10-27 RenderScale v2: a native frame
			vec("TESR_CameraData", 5, farZ, 0, 0);
			vec("TESR_DepthConstants", 0, 0, 0, 0);
			// (UNOFFICIAL 2026-10-27: CLOUD_CAMX / CLOUD_CAMY move the camera, in metres: m8 stands 5 km from the storm of cell 0, 0)
			vec("TESR_CameraPosition", (float)atof(getenv("CLOUD_CAMX") ? getenv("CLOUD_CAMX") : "0") * 70.0f,
				(float)atof(getenv("CLOUD_CAMY") ? getenv("CLOUD_CAMY") : "0") * 70.0f, 1000, 1);
			vec("TESR_SunDirection", sun.x, sun.y, sun.z, 1);
			vec("TESR_SunColor", v.sunR, v.sunG, v.sunB, 1);
			vec("TESR_SunAmount", v.day, 0, 0, 1);
			vec("TESR_SkyColor", v.skyR, v.skyG, v.skyB, 1);
			vec("TESR_SkyLowColor", v.horR, v.horG, v.horB, 1);
			vec("TESR_HorizonColor", v.horR, v.horG, v.horB, 1);
			auto env = [](const char* n, float d) { const char* e = getenv(n); return e ? (float)atof(e) : d; };
			// CLOUD_USER=1: the user's own toml (2026-10-20) instead of the defaults
			const bool user = getenv("CLOUD_USER") != NULL;
			const float haze = user ? 15555.0f : 30000.0f, bright = user ? 3.5f : 1.0f;
			// UNOFFICIAL 2026-10-25 Towering (CLOUD_TOWER 0-1, CLOUD_TOWERH 1-4): as VolumetricCloudsEffect::UpdateConstants
			const float towerFade = 1.0f - (std::min)((std::max)((env("CLOUD_COVER", v.coverage) - 0.6f) / 0.3f, 0.0f), 1.0f);
			const float towering = env("CLOUD_TOWER", 0.0f) * towerFade * towerFade * (3.0f - 2.0f * towerFade);
			const float towerScale = 1.0f + (env("CLOUD_TOWERH", 5.0f) - 1.0f) * towering;
			// (UNOFFICIAL 2026-10-27: CLOUD_STEPS the Steps setting, CLOUD_MAXSTEPS the cap: 64 as the D3D9 shader, higher for a test copy
			// with a longer loop, as the Vulkan march's count)
			const float steps = (std::min)(env("CLOUD_MAXSTEPS", 64.0f), roundf(env("CLOUD_STEPS", user ? 24.0f : 48.0f) * (1.0f + env("CLOUD_TOWERSTEPS", 0.25f) * (towerScale - 1.0f))));
			vec("TESR_CloudsTower", 1.0f / towerScale, towering, env("CLOUD_ANVIL", 1.0f), 0.0f);
			vec("TESR_CloudsShape", user ? 5555.0f : 1500.0f, (user ? 3333.0f : 1800.0f) * towerScale, env("CLOUD_COVER", v.coverage), env("CLOUD_DENSITY", user ? 0.5f : 1.0f));
			vec("TESR_CloudsLook", env("CLOUD_SHARP", user ? 1.0f : 2.0f), env("CLOUD_MIP", user ? 1.0f : 0.0f), env("CLOUD_WARP", user ? 0.0f : 1.0f), env("CLOUD_SMOOTH", user ? 0.0f : 0.5f));
			vec("TESR_CloudsNoise", env("CLOUD_SHAPE", user ? 33333.0f : 9000.0f), env("CLOUD_DETAIL", user ? 1888.0f : 500.0f), env("CLOUD_EROSION", user ? 0.33f : 0.5f), user ? 82222.0f : 30000.0f);
			vec("TESR_CloudsDetail", env("CLOUD_FINE", user ? 0.2f : 0.0f), user ? 4221.5f : 140.0f, 256.0f, env("CLOUD_CUMULUS", 1.0f));
			vec("TESR_CloudsMotion", 0.0f, 0.0f, 0.0f, env("CLOUD_SHARPNESS", user ? 2.0f : 0.5f));
			vec("TESR_CloudsLight", 1.4f, user ? 0.4f : 0.7f, user ? 0.95f : 0.6f, v.rain);
			effect->SetMatrix("TESR_ViewTransform", &view);
			effect->SetMatrix("TESR_ProjectionTransform", &proj);
			effect->SetMatrix("TESR_CloudsPrevViewProj", &viewProj);

			device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			device->SetRenderState(D3DRS_ZENABLE, FALSE);
			device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
			int read = 0;
			LARGE_INTEGER f0, f1, freq; QueryPerformanceFrequency(&freq);
			// UNOFFICIAL 2026-10-27 (tower grain): CLOUD_HISTORY the History setting; CLOUD_SAVEALL=N also composites and saves each of
			// the last N frames (<name><tag>_fNNN.png), for the grain left after the history (CLOUD_WIND=0: still clouds)
			const float historyWeight = env("CLOUD_HISTORY", 0.8f), windScale = env("CLOUD_WIND", 1.0f);
			const int saveAll = getenv("CLOUD_SAVEALL") ? atoi(getenv("CLOUD_SAVEALL")) : 0;
			auto composite = [&](const std::string& path) {
				Check(device->BeginScene(), "begin");
				device->SetRenderTarget(0, finalS.Get());
				D3DVIEWPORT9 full = { 0, 0, W, H, 0, 1 }; device->SetViewport(&full);
				UINT passes;
				effect->SetTechnique(effect->GetTechniqueByName("Composite"));
				effect->Begin(&passes, 0); effect->BeginPass(0);
				device->SetTexture(0, scene.Get()); device->SetTexture(1, depth.Get()); device->SetTexture(6, NULL); device->SetTexture(7, history[read].Get());
				quad(W, H);
				effect->EndPass(); effect->End();
				device->EndScene();
				Check(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, finalS.Get(), NULL, NULL), "save");
			};
			QueryPerformanceCounter(&f0);
			for (int frame = 0; frame < frames; frame++) {
				vec("TESR_CloudsWind", frame * 2.0f * windScale, frame * 1.0f * windScale, frame * 0.5f * windScale, v.typeBias);
				vec("TESR_CloudsMarch", steps, 80000, (float)frame, haze);
				vec("TESR_CloudsLayout", 1.0f / LW, 1.0f / LH, frame ? historyWeight : 0.0f, bright);
				static const float order[4][2] = { {0, 0}, {1, 1}, {1, 0}, {0, 1} };
				vec("TESR_CloudsBlocks", 1.0f / BW, 1.0f / BH, order[frame % 4][0], order[frame % 4][1]);
				Check(device->BeginScene(), "begin");
				UINT passes;
				// march
				device->SetRenderTarget(0, currentS.Get());
				D3DVIEWPORT9 blocks = { 0, 0, BW, BH, 0, 1 }; device->SetViewport(&blocks);
				D3DVIEWPORT9 low = { 0, 0, LW, LH, 0, 1 };
				effect->SetTechnique(effect->GetTechniqueByName(getenv("CLOUD_TOWER") && atof(getenv("CLOUD_TOWER")) > 0.0 ? "MarchTowers" : "March"));
				effect->Begin(&passes, 0); effect->BeginPass(0);
				device->SetTexture(0, scene.Get()); device->SetTexture(1, depth.Get()); device->SetTexture(2, shape.Get());
				device->SetTexture(3, detail.Get()); device->SetTexture(4, weather.Get()); device->SetTexture(5, blue.Get());
				device->SetTexture(6, NULL); device->SetTexture(7, history[read].Get()); device->SetTexture(8, noiseVolume.Get());
				quad(BW, BH);
				effect->EndPass(); effect->End();
				// temporal
				device->SetRenderTarget(0, historyS[1 - read].Get()); device->SetViewport(&low);
				effect->SetTechnique(effect->GetTechniqueByName("Temporal"));
				effect->Begin(&passes, 0); effect->BeginPass(0);
				device->SetTexture(6, current.Get()); device->SetTexture(7, history[read].Get());
				quad(LW, LH);
				effect->EndPass(); effect->End();
				read = 1 - read;
				device->EndScene();
				if (saveAll > 0 && frame >= frames - saveAll && frame < frames - 1) {
					char suffix[16]; sprintf_s(suffix, "_f%03d", frame);
					composite(out + "\\" + v.name + (getenv("CLOUD_TAG") ? getenv("CLOUD_TAG") : "") + suffix + ".png");
				}
			}
			// wait for the GPU, for the timing (the march and temporal passes only when no frame is saved on the way)
			ComPtr<IDirect3DQuery9> query; device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
			query->Issue(D3DISSUE_END); while (query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
			QueryPerformanceCounter(&f1);
			const std::string path = out + "\\" + v.name + (getenv("CLOUD_TAG") ? getenv("CLOUD_TAG") : "") + ".png";
			composite(path);
			std::printf("%s  (%d frames, %.2f ms per frame incl. upload)\n", path.c_str(), frames,
				(f1.QuadPart - f0.QuadPart) * 1000.0 / freq.QuadPart / frames);
			(void)lin; (void)gam;
		}
		DestroyWindow(window);
		return 0;
	}
	catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
