// UNOFFICIAL (2026-10-12): offline checks of the in-game effect self-test's device and number half (src\core\EffectSelfTestCore.h)
// on a real D3D9 device with made-up frames: the readback (GPU halving and CPU fallback, formats, multisampling, odd sizes), the
// flicker and light numbers, the light/ring masks and their colour figures, the PNG sheets, and the frame schedule.
// Usage: build\selftest-test\effect_selftest.exe [output folder]   (exit code 0 = all checks passed)
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/EffectSelfTestCore.h"

using namespace SelfTestCore;

static IDirect3DDevice9* device = nullptr;
static int failures = 0;

static void Check(bool ok, const char* what, double got = 0, double want = 0) {
	std::printf("%s: %s (got %.6f, want %.6f)\n", ok ? "PASS" : "FAIL", what, got, want);
	if (!ok) failures++;
}
static bool Near(double a, double b, double tolerance) { return fabs(a - b) <= tolerance; }

// A render target filled from rgb (w x h x 3, alpha 1).
struct Target {
	IDirect3DSurface9* surface = nullptr;
	unsigned w = 0, h = 0;
	D3DFORMAT format = D3DFMT_A16B16G16R16F;
	D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_NONE;
	IDirect3DSurface9* staging = nullptr;	// default pool plain surface (UpdateSurface target), then StretchRect
	IDirect3DSurface9* upload = nullptr;	// system memory
	bool Make(unsigned width, unsigned height, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE) {
		w = width; h = height; format = f; samples = ms;
		return SUCCEEDED(device->CreateRenderTarget(w, h, f, ms, 0, FALSE, &surface, NULL)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, f, D3DPOOL_DEFAULT, &staging, NULL)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, f, D3DPOOL_SYSTEMMEM, &upload, NULL));
	}
	void Fill(const std::vector<float>& rgb) {
		D3DLOCKED_RECT lock;
		upload->LockRect(&lock, NULL, 0);
		for (unsigned y = 0; y < h; y++) {
			unsigned char* line = (unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
			for (unsigned x = 0; x < w; x++) {
				const float* c = &rgb[((size_t)y * w + x) * 3];
				if (format == D3DFMT_A16B16G16R16F) { const float v[4] = { c[0], c[1], c[2], 1.0f }; D3DXFloat32To16Array((D3DXFLOAT16*)line + x * 4, v, 4); }
				else { unsigned* p = (unsigned*)line; p[x] = 0xFF000000u | (Byte(c[0]) << 16) | (Byte(c[1]) << 8) | Byte(c[2]); }
			}
		}
		upload->UnlockRect();
		device->UpdateSurface(upload, NULL, staging, NULL);
		device->StretchRect(staging, NULL, surface, NULL, D3DTEXF_NONE);
	}
	void Release() { for (auto** s : { &surface, &staging, &upload }) if (*s) { (*s)->Release(); *s = nullptr; } }
};

// fp16 rounding of a value (what the target stores).
static float Half(float v) { D3DXFLOAT16 h; D3DXFloat32To16Array(&h, &v, 1); float o; D3DXFloat16To32Array(&o, &h, 1); return o; }

static std::vector<float> Uniform(unsigned w, unsigned h, float r, float g, float b) {
	std::vector<float> v((size_t)w * h * 3);
	for (size_t i = 0; i < (size_t)w * h; i++) { v[i * 3] = r; v[i * 3 + 1] = g; v[i * 3 + 2] = b; }
	return v;
}

static const unsigned W = 256, H = 160;	// quarter: 64 x 40

static std::vector<float> Static(int, unsigned w, unsigned h) {
	std::vector<float> v((size_t)w * h * 3);
	for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++)
		for (int c = 0; c < 3; c++) v[((size_t)y * w + x) * 3 + c] = 0.2f + 2.0f * (float)((x * 7 + y * 13 + c * 5) % 17) / 17.0f;
	return v;
}
// A 25% patch (left half of the top half, block aligned) alternating +-0.02 round 1.0.
static std::vector<float> Noisy(int f, unsigned w, unsigned h) {
	std::vector<float> v = Uniform(w, h, 1.0f, 1.0f, 1.0f);
	const float d = (f & 1) ? 0.02f : -0.02f;
	for (unsigned y = 0; y < h / 2; y++) for (unsigned x = 0; x < w / 2; x++) for (int c = 0; c < 3; c++) v[((size_t)y * w + x) * 3 + c] += d;
	return v;
}
static std::vector<float> Grey(int, unsigned w, unsigned h) { return Uniform(w, h, 0.5f, 0.5f, 0.5f); }
// x1.6 inside a block-aligned 64 x 64 square (256 quarter pixels of 2560).
static std::vector<float> Added(int, unsigned w, unsigned h) {
	std::vector<float> v = Uniform(w, h, 0.5f, 0.5f, 0.5f);
	for (unsigned y = 32; y < 96; y++) for (unsigned x = 96; x < 160; x++) for (int c = 0; c < 3; c++) v[((size_t)y * w + x) * 3 + c] = 0.8f;
	return v;
}
// A red light 64 x 64 (16 x 16 quarter px) at 96..160 / 48..112 on a dark red-grey wall; config 1 adds 0.3 to all channels
// on the light and within 32 px of it.
static std::vector<float> Sign(int, unsigned w, unsigned h) {
	std::vector<float> v = Uniform(w, h, 0.3f, 0.1f, 0.1f);
	for (unsigned y = 48; y < 112; y++) for (unsigned x = 96; x < 160; x++) { float* c = &v[((size_t)y * w + x) * 3]; c[0] = 4.0f; c[1] = 0.2f; c[2] = 0.2f; }
	return v;
}
static std::vector<float> SignSpill(int f, unsigned w, unsigned h) {
	std::vector<float> v = Sign(f, w, h);
	for (unsigned y = 16; y < 144; y++) for (unsigned x = 64; x < 192; x++) for (int c = 0; c < 3; c++) v[((size_t)y * w + x) * 3 + c] += 0.3f;
	return v;
}

// Runs frames through a Series into a block's means.
static BlockStats RunBlock(Reader& reader, Target& target, int frames, std::vector<float> (*frame)(int, unsigned, unsigned)) {
	Series s;
	std::vector<float> rgb;
	for (int f = 0; f < frames; f++) {
		target.Fill(frame(f, target.w, target.h));
		if (!reader.Read(device, target.surface, rgb)) { std::printf("read failed: %s\n", reader.error); failures++; break; }
		s.Add(rgb, reader.width, reader.height);
	}
	BlockStats b;
	b.From(s);
	return b;
}
static std::vector<float> Level(unsigned w, unsigned h, float v) { return Uniform(w, h, v, v, v); }
static std::vector<float> L049(int, unsigned w, unsigned h) { return Level(w, h, 0.49f); }
static std::vector<float> L051(int, unsigned w, unsigned h) { return Level(w, h, 0.51f); }
static std::vector<float> L052(int, unsigned w, unsigned h) { return Level(w, h, 0.52f); }
// 0.53 with region (row 1, column 1) much brighter by itself.
static std::vector<float> L053Drift(int, unsigned w, unsigned h) {
	std::vector<float> v = Level(w, h, 0.53f);
	for (unsigned y = 0; y < h / 8; y++) for (unsigned x = 0; x < w / 8; x++) for (int c = 0; c < 3; c++) v[((size_t)y * w + x) * 3 + c] = 1.5f;
	return v;
}

int main(int argc, char** argv) {
	const std::string out = argc > 1 ? argv[1] : ".";
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "SelfTestCheck"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("SelfTestCheck", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) { std::puts("no D3D9 device"); return 1; }

	// 1. Readback: the GPU halving against the CPU box average, static picture; then the format / multisample / odd-size paths.
	{
		Target t; t.Make(W, H, D3DFMT_A16B16G16R16F);
		t.Fill(Static(0, W, H));
		Reader gpu, cpu; cpu.forceCpu = true;
		std::vector<float> a, b;
		const bool ra = gpu.Read(device, t.surface, a), rb = cpu.Read(device, t.surface, b);
		Check(ra && rb && gpu.width == W / 4 && gpu.height == H / 4 && cpu.width == W / 4, "fp16 target read at a quarter size by both paths", gpu.width, W / 4);
		std::printf("     (GPU halving %s)\n", gpu.cpu ? "NOT available on this device/format: the CPU path was used" : "used");
		double worst = 0;
		for (size_t i = 0; i < a.size() && i < b.size(); i++) worst = (std::max)(worst, fabs(a[i] - b[i]) / (std::max)(0.05, (double)fabs(b[i])));
		Check(worst < 0.004, "GPU halving = 4x4 box average (largest relative difference)", worst, 0.004);
		// the CPU box average is exact: compare one block with a hand sum of the fp16 values
		const std::vector<float> src = Static(0, W, H);
		double sum = 0; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) sum += Half(src[((size_t)(4 + y) * W + 8 + x) * 3 + 1]);
		Check(Near(b[((size_t)1 * (W / 4) + 2) * 3 + 1], sum / 16, 1e-6), "CPU path: a block is the mean of its 16 pixels", b[((size_t)1 * (W / 4) + 2) * 3 + 1], sum / 16);
		gpu.Release(); cpu.Release(); t.Release();

		Target odd; odd.Make(803, 451, D3DFMT_A8R8G8B8);
		odd.Fill(Uniform(803, 451, 0.25f, 0.5f, 0.75f));
		Reader r; std::vector<float> c;
		const bool ok = r.Read(device, odd.surface, c);
		Check(ok && r.width == 200 && r.height == 112 && Near(c[1000 * 3 + 1], 128 / 255.0, 0.01), "8-bit, odd size 803x451: 200x112, green 0.5", ok ? c[1000 * 3 + 1] : -1, 128 / 255.0);
		r.Release(); odd.Release();

		DWORD quality = 0;
		if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A16B16G16R16F, TRUE, D3DMULTISAMPLE_4_SAMPLES, &quality))) {
			Target ms; ms.Make(W, H, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_4_SAMPLES);
			ms.Fill(Uniform(W, H, 1.5f, 0.5f, 0.25f));
			Reader m; std::vector<float> d;
			const bool okm = m.Read(device, ms.surface, d);
			Check(okm && m.width == W / 4 && Near(d[100 * 3], 1.5, 0.002), "multisampled fp16 target: resolved and read", okm ? d[100 * 3] : -1, 1.5);
			m.Release(); ms.Release();
		}
		else std::puts("SKIP: 4x multisampling of fp16 not available");
	}

	Target t; t.Make(W, H, D3DFMT_A16B16G16R16F);
	// 2. Flicker: a still picture has none; a 25% patch alternating +-0.02 round 1.0 has |change| 0.04 a frame. Flicker is taken
	// against the REFERENCE's brightness: the same patch on a picture twice as bright as the reference reads twice the flicker.
	{
		Reader reader;
		PointData p;
		FirstReference(p, RunBlock(reader, t, 12, Static), true);
		FinishRun(p);
		const Figures& r = p.results[0].all;
		Check(p.results[0].valid && r.flicker == 0.0f && r.flickerPixels == 0.0f, "still picture: flicker exactly 0", r.flicker, 0);
		Check(Near(r.whole, 1.0, 1e-9) && r.brighter10 == 0 && r.darker10 == 0, "reference against itself: ratio 1, nothing brighter or darker", r.whole, 1);
		p.Reset();
		FirstReference(p, RunBlock(reader, t, 40, Noisy), true);
		FinishRun(p);
		const Figures& n = p.results[0].all;
		const double change = Half(1.02f) - Half(0.98f);
		Check(Near(n.flicker, 25.0 * change, 0.01 * 25.0 * change), "flicker of a 25% patch alternating +-0.02 (% a frame)", n.flicker, 25.0 * change);
		Check(Near(n.flickerPixels, 25.0, 0.01), "flickering pixels: the patch's 25%", n.flickerPixels, 25.0);
		p.Reset();
		FirstReference(p, RunBlock(reader, t, 3, Grey), true);	// reference 0.5
		ConfigurationBlock(p, RunBlock(reader, t, 40, Noisy));		// 1.0 with the noisy patch
		NextReference(p, RunBlock(reader, t, 3, Grey), 1);
		FinishRun(p);
		const Figures& b = p.results[1].all;
		const double want = 100.0 * (0.25 * change) / Half(0.5f);
		Check(Near(b.flicker, want, 0.01 * want), "flicker against the reference's brightness (0.5), not the configuration's own (1.0)", b.flicker, want);
		Check(b.refFlicker == 0.0f && b.refFlickerPixels == 0.0f, "the reference's own flicker: 0", b.refFlicker, 0);
		reader.Release();
	}
	// 3. Bracketing: references 0.49, 0.51, 0.53 (an even drift), configuration 1 = 0.50 with a x1.6 square (256 of 2560 quarter
	// pixels), configuration 2 = 0.52. Against the mean of the references round each, the drift cancels. One region of the last
	// reference is made brighter by itself: it is left out of the steady figures.
	{
		Reader reader;
		PointData p;
		FirstReference(p, RunBlock(reader, t, 3, L049), true);
		ConfigurationBlock(p, RunBlock(reader, t, 3, Added));
		NextReference(p, RunBlock(reader, t, 3, L051), 1);
		ConfigurationBlock(p, RunBlock(reader, t, 3, L052));
		NextReference(p, RunBlock(reader, t, 3, L053Drift), 2);
		FinishRun(p);
		const Result& r = p.results[1];
		const double bracket = 0.5 * (Half(0.49f) + Half(0.51f));
		Check(Near(r.ratio[0][0], Half(0.5f) / bracket, 1e-4), "drift cancelled: an unchanged region reads 1 against the references round it", r.ratio[0][0], Half(0.5f) / bracket);
		Check(Near(r.stable.brighter50, 100.0 * 256 / (2560 - 40), 0.05), "share 50%+ brighter (the square, among the steady regions)", r.stable.brighter50, 100.0 * 256 / 2520);
		Check(Near(r.ratio[2][3], Half(0.8f) / bracket, 1e-4), "region ratio inside the square", r.ratio[2][3], Half(0.8f) / bracket);
		Check(p.excludedCount == 1 && p.excluded[0], "the region that changed by itself is found and left out", p.excludedCount, 1);
		const Result& r2 = p.results[2];
		const double b2 = 0.5 * (Half(0.51f) + Half(0.53f));
		Check(Near(r2.stable.whole, Half(0.52f) / b2, 1e-4) && r2.all.whole < r2.stable.whole - 0.005, "steady figures leave it out (all regions are pulled down by it)", r2.stable.whole, Half(0.52f) / b2);
		const double noise = ReferenceRatio(p, 1, true), noiseWant = Half(0.51f) / (0.5 * (Half(0.49f) + Half(0.53f)));
		Check(Near(noise, noiseWant, 1e-4) && ReferenceRatio(p, 1, false) < noise, "noise floor: the middle reference against the two round it (steady regions)", noise, noiseWant);
		// the light picture, read back: tile 1 (x 64..127, y 0..39) red at the square's centre, grey outside
		const std::string path = out + "\\selftest_light.png";
		Check(SaveLightSheet(path.c_str(), p, 3), "light picture saved", 1, 1);
		IDirect3DSurface9* back = nullptr;
		device->CreateOffscreenPlainSurface(64 * 4, 40 * 4, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &back, NULL);
		if (back && SUCCEEDED(D3DXLoadSurfaceFromFileA(back, NULL, NULL, path.c_str(), NULL, D3DX_FILTER_NONE, 0, NULL))) {
			D3DLOCKED_RECT lock; back->LockRect(&lock, NULL, D3DLOCK_READONLY);
			const unsigned inside = ((unsigned*)((unsigned char*)lock.pBits + 16 * lock.Pitch))[64 + 32];
			const unsigned outside = ((unsigned*)((unsigned char*)lock.pBits + 35 * lock.Pitch))[64 + 5];
			back->UnlockRect();
			const unsigned ir = (inside >> 16) & 255, ig = (inside >> 8) & 255, orr = (outside >> 16) & 255, og = (outside >> 8) & 255, ob = outside & 255;
			Check(ir > 230 && ig < 80 && orr == og && og == ob, "picture read back: red where brighter, grey elsewhere", ir, 255);
		}
		else Check(false, "picture read back", 0, 1);
		if (back) back->Release();
		Check(SaveFlickerSheet((out + "\\selftest_flicker.png").c_str(), p, 3), "flicker picture saved", 1, 1);
		reader.Release();
	}
	// 4. Lights and the ring round them: a 16x16 (quarter px) red light; the spill adds 0.3 everywhere within 8 px of it.
	{
		Reader reader;
		PointData p;
		FirstReference(p, RunBlock(reader, t, 2, Sign), true);
		BuildMasks(p, 1.0f);	// linear threshold: the red light (4.0 encoded) counts, the wall (0.3) does not
		size_t count[3] = {};
		for (unsigned char m : p.mask) count[m]++;
		Check(p.masks && count[1] == 256 && count[2] == 32 * 32 - 256, "masks: 256 light pixels, a ring of 768", (double)count[1] * 10000 + count[2], 256 * 10000 + 768);
		ConfigurationBlock(p, RunBlock(reader, t, 2, SignSpill));
		NextReference(p, RunBlock(reader, t, 2, Sign), 1);
		FinishRun(p);
		const Figures& r = p.results[1].stable;
		auto sat = [](float a, float b, float c) { float x[3] = { Half(a), Half(b), Half(c) }; return Saturation(x); };
		auto luma = [](float a, float b, float c) { float x[3] = { Half(a), Half(b), Half(c) }; return Luma(x); };
		const double ringColour = sat(0.6f, 0.4f, 0.4f) / sat(0.3f, 0.1f, 0.1f), lightColour = sat(4.3f, 0.5f, 0.5f) / sat(4.0f, 0.2f, 0.2f);
		const double ringRatio = luma(0.6f, 0.4f, 0.4f) / luma(0.3f, 0.1f, 0.1f), lightRatio = luma(4.3f, 0.5f, 0.5f) / luma(4.0f, 0.2f, 0.2f);
		Check(r.masks && Near(r.ringColour, ringColour, 1e-4), "ring: colour kept", r.ringColour, ringColour);
		Check(Near(r.ringRatio, ringRatio, 1e-4), "ring: brightness ratio", r.ringRatio, ringRatio);
		Check(Near(r.lightsColour, lightColour, 1e-4), "lights: colour kept", r.lightsColour, lightColour);
		Check(Near(r.lightsRatio, lightRatio, 1e-4), "lights: brightness ratio", r.lightsRatio, lightRatio);
		const Figures& self = p.results[0].stable;
		Check(Near(self.lightsShare, 100.0 * 256 / 2560, 1e-4) && Near(self.ringShare, 100.0 * 768 / 2560, 1e-4) && self.lightsRatio == 1.0f,
			"the reference's own mask figures: shares of the picture, ratio 1", self.lightsShare, 10.0);
		Check(SaveLightSheet((out + "\\selftest_masks.png").c_str(), p, 2), "mask picture saved", 1, 1);
		reader.Release();
	}
	t.Release();
	// 5. Real size (the 2026-10-12 hang): 2560x1440 read at a quarter = 640x360 per point, 9 configurations (8 tested + the
	// reference) and 9 reference blocks, masks. The comparisons, the end of the run and both sheets must be fast (the old light
	// sheet re-summed the whole picture for every pixel: about 330 s).
	{
		const unsigned w = 640, h = 360;
		const size_t n = (size_t)w * h;
		auto block = [&](float scale, int seed) {
			BlockStats b; b.frames = 30; b.width = w; b.height = h;
			b.rgb.resize(n * 3); b.luma.resize(n); b.flicker.resize(n);
			for (size_t i = 0; i < n; i++) {
				for (int c = 0; c < 3; c++) b.rgb[i * 3 + c] = scale * (0.05f + 3.0f * (float)((i * 7 + c * 13 + seed) % 101) / 101.0f);
				b.luma[i] = Luma(&b.rgb[i * 3]); b.flicker[i] = 0.001f * (float)((i + seed) % 50);
			}
			return b;
		};
		PointData p;
		ULONGLONG t0 = GetTickCount64();
		FirstReference(p, block(1.0f, 0), true);
		BuildMasks(p, 1.0f);
		for (int c = 1; c <= 8; c++) { ConfigurationBlock(p, block(1.0f + 0.05f * c, c)); NextReference(p, block(1.0f, 0), c); }
		FinishRun(p);
		ULONGLONG t1 = GetTickCount64();
		std::vector<unsigned> sheet;
		const bool light = BuildLightSheet(sheet, p, 9, 20.0);
		ULONGLONG t2 = GetTickCount64();
		const bool lightPng = light && SavePng((out + "\\selftest_real_light.png").c_str(), w * 4, h * 4, sheet);
		ULONGLONG t3 = GetTickCount64();
		const bool flicker = BuildFlickerSheet(sheet, p, 9, 20.0);
		ULONGLONG t4 = GetTickCount64();
		const bool flickerPng = flicker && SavePng((out + "\\selftest_real_flicker.png").c_str(), w * 4, h * 4, sheet);
		ULONGLONG t5 = GetTickCount64();
		std::printf("     (640x360, 8 configurations + 9 references: comparisons and the end of the run %llu ms in all; light sheet %llu ms, PNG %llu ms; flicker sheet %llu ms, PNG %llu ms)\n",
			t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4);
		Check(p.references == 9 && p.results[8].valid, "real size: 9 reference blocks, 8 configurations compared", p.references, 9);
		Check((t1 - t0) < 1500, "real size: all comparisons + the end of the run under 1.5 s in all (ms)", (double)(t1 - t0), 1500);
		Check(light && flicker && (t2 - t1) < 2000 && (t4 - t3) < 2000, "real size: both sheets built in under 2 s each (ms, light)", (double)(t2 - t1), 2000);
		Check(lightPng && flickerPng && (t3 - t2) < 3000 && (t5 - t4) < 3000, "real size: both PNGs written in under 3 s each (ms, light)", (double)(t3 - t2), 3000);
		PointData broken = std::move(p); broken.refRgb.resize(10);
		Check(!BuildLightSheet(sheet, broken, 9, 20.0) && !BuildFlickerSheet(sheet, broken, 9, 20.0), "sizes that do not match: no sheet (no out-of-range reads)", 0, 0);
	}
	// 6. The schedule of a run: 9 configurations (the reference + 8) = 17 blocks of 60 settle + 30 read.
	{
		Schedule s; s.settle = 60; s.measure = 30; s.Standard(9);
		int measured[9] = {}, firsts = 0, lasts = 0, steps = 0, referenceBlocks = 0;
		for (s.step = 0; !s.Done(); s.step++) {
			steps++;
			if (s.Measuring()) measured[s.Config()]++;
			if (s.FirstMeasure()) firsts++;
			if (s.LastMeasure() && s.Config() == 0) referenceBlocks++;
			if (s.LastMeasure()) lasts++;
		}
		bool even = measured[0] == 9 * 30;
		for (int c = 1; c < 9; c++) even = even && measured[c] == 30;
		Check(steps == 17 * 90 && even && firsts == 17 && lasts == 17 && referenceBlocks == 9, "schedule: 1530 frames, 17 blocks, 9 of them spill off, 30 read each", steps, 1530);
		s.step = 89; const bool ref = s.Config() == 0; s.step = 90; const bool one = s.Config() == 1 && !s.Measuring(); s.step = 180; const bool back = s.Config() == 0;
		s.step = 15 * 90; const bool eight = s.Config() == 8; s.step = 16 * 90; const bool last = s.Config() == 0;
		Check(ref && one && back && eight && last, "schedule: off, 1, off, 2, ..., 8, off", s.Config(), 0);
		// re-runs appended after the end: configuration 2 and a reference
		s.step = 17 * 90; s.Add(2); s.Add(0);
		const bool more = !s.Done() && s.Config() == 2 && s.Block() == 17; s.step = 18 * 90; const bool after = s.Config() == 0; s.step = 19 * 90;
		Check(more && after && s.Done() && s.Blocks() == 19, "schedule: a re-run (2, off) appended after the end runs, then the run ends", s.Blocks(), 19);
	}
	// 7. Intrusion (critic-review-6): something walking through a configuration block. 64x40 pictures, 4 configurations.
	{
		const unsigned w = 64, h = 40;
		const size_t n = (size_t)w * h;
		auto block = [&](float level, float spillPatch, float walker, float walkerFlicker) {
			// level everywhere; +spillPatch (the spill's light) in the top half; a "walker" darker by walker and flickering by
			// walkerFlicker in a 16x16 patch at the bottom left (where the spill does not reach)
			BlockStats b; b.frames = 30; b.width = w; b.height = h;
			b.rgb.assign(n * 3, level); b.luma.assign(n, level); b.flicker.assign(n, 0.0005f);
			for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
				const size_t i = (size_t)y * w + x;
				float v = level + (y < h / 2 ? spillPatch : 0.0f);
				if (y >= h - 16 && x < 16) { v *= 1.0f - walker; b.flicker[i] = walkerFlicker; }
				for (int c = 0; c < 3; c++) b.rgb[i * 3 + c] = v;
				b.luma[i] = v;
			}
			return b;
		};
		PointData p;
		FirstReference(p, block(0.5f, 0, 0, 0), true);
		ConfigurationBlock(p, block(0.5f, 0.1f, 0, 0)); NextReference(p, block(0.5f, 0, 0, 0), 1);			// clean
		ConfigurationBlock(p, block(0.5f, 0.1f, 0.4f, 0.0005f)); NextReference(p, block(0.5f, 0, 0, 0), 2);	// a dark walker: 10% of the picture 40% darker
		ConfigurationBlock(p, block(0.5f, 0.1f, 0.0f, 0.01f)); NextReference(p, block(0.5f, 0, 0, 0), 3);	// a flickering walker, no darker
		FinishRun(p);
		const Intrusion clean = CheckIntrusion(p, 1, 4), dark = CheckIntrusion(p, 2, 4), flick = CheckIntrusion(p, 3, 4);
		std::printf("     (intrusion: clean darker %.1f%% flicker x%.2f unlit %.0f%%; dark walker %.1f%% x%.2f; flickering walker %.1f%% x%.2f)\n",
			clean.darker, clean.flickerRatio, clean.unlitShare, dark.darker, dark.flickerRatio, flick.darker, flick.flickerRatio);
		Check(clean.valid && !clean.flagged, "intrusion: a clean configuration is not flagged", clean.darker, 0);
		Check(dark.flagged && dark.darker > 3.0f, "intrusion: a walker making 10% of the picture darker is flagged", dark.darker, 10);
		Check(flick.flagged && flick.flickerRatio > 2.0f && flick.darker <= 3.0f, "intrusion: a flickering walker where the spill does not reach is flagged", flick.flickerRatio, 2);
		Check(NoiseLevel(p) < 1e-6, "noise level of identical references: 0", NoiseLevel(p), 0);
		// critic-notes 10: a bloom-off reference (another kind) between spill-off ones stays out of the noise floor and drift,
		// and a configuration between two bloom-off references is compared with them.
		PointData q;
		FirstReference(q, block(0.5f, 0, 0, 0), true);
		NextReference(q, block(0.4f, 0, 0, 0), 0, false);				// bloom-off reference: darker, not counted
		ConfigurationBlock(q, block(0.44f, 0, 0, 0));
		NextReference(q, block(0.4f, 0, 0, 0), 11, false);				// "your settings, bloom off" against the bloom-off references
		NextReference(q, block(0.5f, 0, 0, 0), 0, true);				// back to spill-off references
		FinishRun(q);
		Check(q.references == 2 && NoiseLevel(q) < 1e-6 && q.excludedCount == 0, "bloom-off references stay out of the noise floor and the drift",
			q.references, 2);
		Check(q.results[11].valid && fabs(q.results[11].all.whole - 1.1) < 1e-3, "a configuration between bloom-off references is compared with them (x1.1)",
			q.results[11].all.whole, 1.1);
	}
	// 7. critic-review-14 part 1. (a) The frame-scoped overrides: 3 frames of the test's tone-mapping and Bounce Light values, each
	// put back; every constant and member bit-identical afterwards (a value set twice comes back as before the first Set).
	{
		struct Constants { D3DXVECTOR4 ToneMapping, BloomData, HDRData, LotteData; } k = {
			{ 1.0f, 0.37f, 0.11f, -0.0f }, { 0.81f, 1.13f, 0.5f, 6.2f }, { 10.0f, 1.07f, 0.93f, 2.2f }, { 1.21f, 1.0f, 1.18f, 0.7f } };
		struct FakeBounce {
			float neonSpill = 2.0f, spillReach = 0.25f, spillLightLevel = 3.0f, spillNear = 0.5f, spillFar = 0.5f, spillCap = 16.0f, spillMemorySeconds = 1.0f;
			float spillSeparateDepth = 0.0f, spillHardEdges = 1.0f, spillHoldAfterTurn = 0.0f, spillCapSceneLight = 0.0f, spillMaskSpillOnly = 0.0f;
			int spillDebug = 1;
		} bounce;
		Constants before; memcpy(&before, &k, sizeof(k));
		FakeBounce bounceBefore = bounce;
		BounceValues user; Read(&bounce, user);
		bool same = true;
		for (int frame = 0; frame < 3; frame++) {
			FrameOverride o;
			o.Set(&k.BloomData.x, 0.0f);
			o.Set(&k.LotteData.x, 3.3f); o.Set(&k.LotteData.y, 4.4f); o.Set(&k.LotteData.z, 5.5f); o.Set(&k.LotteData.w, 0.99f);
			o.Set(&k.HDRData.x, 9.0f); o.Set(&k.HDRData.x, 1.0f);	// set twice
			o.Set(&k.ToneMapping.x, 1.5f);
			BounceValues v = user; v.spillDebug = 0; v.spillDebug = 2; v.spillSeparateDepth = 1.0f; v.neonSpill = 0.0f;
			Write(&bounce, v);
			same = same && k.HDRData.x == 1.0f && k.BloomData.x == 0.0f && bounce.spillDebug == 2;	// the test's values are in for the frame
			o.Restore();
			Write(&bounce, user);
			same = same && !o.Active() && memcmp(&before, &k, sizeof(k)) == 0 && memcmp(&bounceBefore, &bounce, sizeof(bounce)) == 0;
		}
		Check(same, "overrides: HDRData.x, BloomData.x, ToneMapping.x, LotteData and the Bounce Light members (spillDebug) bit-identical after 3 frames", same, 1);
	}
	// (b) Shots: full size, display-ready BGRA from 8-bit, 10-bit, fp16 and multisampled targets; written as PNG.
	{
		const unsigned sw = 160, sh = 96;
		std::vector<float> pattern((size_t)sw * sh * 3);
		for (unsigned y = 0; y < sh; y++) for (unsigned x = 0; x < sw; x++) {
			float* c = &pattern[((size_t)y * sw + x) * 3];
			c[0] = (x % 64) / 63.0f; c[1] = (y % 32) / 31.0f; c[2] = ((x + y) % 7) / 6.0f;
		}
		auto expected = [&](unsigned x, unsigned y, bool clampHalf) {
			const float* c = &pattern[((size_t)y * sw + x) * 3];
			auto q = [&](float v) { return clampHalf ? Byte(Half(v)) : Byte(v); };
			return 0xFF000000u | (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
		};
		ShotReader reader;
		std::vector<unsigned> bgra; unsigned w = 0, h = 0;
		// 8-bit: the bytes as they are
		Target eight; eight.Make(sw, sh, D3DFMT_X8R8G8B8); eight.Fill(pattern);
		bool ok = reader.Read(device, eight.surface, bgra, w, h) && w == sw && h == sh;
		int wrong = 0; for (unsigned y = 0; ok && y < sh; y++) for (unsigned x = 0; x < sw; x++) if (bgra[(size_t)y * sw + x] != expected(x, y, false)) wrong++;
		Check(ok && wrong == 0, "shot, 8-bit target: full size, every pixel exact", wrong, 0);
		const std::string shot = out + "\\selftest_shot_8bit.png";
		DeleteFileA(shot.c_str());
		const bool saved = ok && SavePng(shot.c_str(), w, h, bgra);
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		Check(saved && GetFileAttributesExA(shot.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 0, "shot written as PNG", info.nFileSizeLow, 1);
		eight.Release();
		// fp16: clamped to 0-1, rounded to bytes (values above 1 white)
		Target half; half.Make(sw, sh, D3DFMT_A16B16G16R16F);
		std::vector<float> hdr = pattern; for (size_t i = 0; i < hdr.size(); i += 7) hdr[i] = 1.7f;
		half.Fill(hdr);
		ok = reader.Read(device, half.surface, bgra, w, h) && w == sw && h == sh;
		wrong = 0;
		for (unsigned y = 0; ok && y < sh; y++) for (unsigned x = 0; x < sw; x++) {
			const float* c = &hdr[((size_t)y * sw + x) * 3];
			const unsigned want = 0xFF000000u | (Byte(Half(c[0])) << 16) | (Byte(Half(c[1])) << 8) | Byte(Half(c[2]));
			if (bgra[(size_t)y * sw + x] != want) wrong++;
		}
		Check(ok && wrong == 0, "shot, fp16 target: clamped to 0-1, every pixel exact", wrong, 0);
		half.Release();
		// 10-bit A2R10G10B10: packed by hand (Target::Fill writes 8-bit or fp16 only)
		IDirect3DSurface9 *rt = nullptr, *staging = nullptr, *upload = nullptr;
		if (SUCCEEDED(device->CreateRenderTarget(sw, sh, D3DFMT_A2R10G10B10, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(sw, sh, D3DFMT_A2R10G10B10, D3DPOOL_DEFAULT, &staging, NULL)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(sw, sh, D3DFMT_A2R10G10B10, D3DPOOL_SYSTEMMEM, &upload, NULL))) {
			D3DLOCKED_RECT lock; upload->LockRect(&lock, NULL, 0);
			for (unsigned y = 0; y < sh; y++) {
				unsigned* line = (unsigned*)((unsigned char*)lock.pBits + (size_t)y * lock.Pitch);
				for (unsigned x = 0; x < sw; x++) {
					const float* c = &pattern[((size_t)y * sw + x) * 3];
					line[x] = 0xC0000000u | ((unsigned)(c[0] * 1023 + 0.5f) << 20) | ((unsigned)(c[1] * 1023 + 0.5f) << 10) | (unsigned)(c[2] * 1023 + 0.5f);
				}
			}
			upload->UnlockRect();
			device->UpdateSurface(upload, NULL, staging, NULL);
			device->StretchRect(staging, NULL, rt, NULL, D3DTEXF_NONE);
			ok = reader.Read(device, rt, bgra, w, h) && w == sw && h == sh;
			wrong = 0;
			for (unsigned y = 0; ok && y < sh; y++) for (unsigned x = 0; x < sw; x++) {
				const float* c = &pattern[((size_t)y * sw + x) * 3];
				auto ten = [](float v) { return Byte((unsigned)(v * 1023 + 0.5f) / 1023.0f); };
				if (bgra[(size_t)y * sw + x] != (0xFF000000u | (ten(c[0]) << 16) | (ten(c[1]) << 8) | ten(c[2]))) wrong++;
			}
			Check(ok && wrong == 0, "shot, 10-bit A2R10G10B10 target: every pixel exact", wrong, 0);
		}
		else std::puts("SKIP: shot, 10-bit target (this device cannot make an A2R10G10B10 render target)");
		for (auto* s : { rt, staging, upload }) if (s) s->Release();
		// multisampled: resolved first
		DWORD quality = 0;
		if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, &quality))) {
			Target ms; ms.Make(sw, sh, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES); ms.Fill(pattern);
			ok = reader.Read(device, ms.surface, bgra, w, h) && w == sw && h == sh && reader.resolve != nullptr;
			wrong = 0; for (unsigned y = 0; ok && y < sh; y++) for (unsigned x = 0; x < sw; x++) if (bgra[(size_t)y * sw + x] != expected(x, y, false)) wrong++;
			Check(ok && wrong == 0, "shot, 4x multisampled 8-bit target: resolved, every pixel exact", wrong, 0);
			ms.Release();
		}
		else std::puts("SKIP: shot, multisampled target (4x not supported for X8R8G8B8 here)");
		reader.Release();
		Check(std::string(FormatName(D3DFMT_A2R10G10B10)) == "A2R10G10B10" && std::string(FormatName(D3DFMT_X8R8G8B8)) == "X8R8G8B8", "format names for the log", 1, 1);
	}
	device->Release(); d3d->Release();
	if (failures) { std::printf("%d effect self-test check(s) FAILED\n", failures); return 1; }
	std::puts("All effect self-test checks passed.");
	return 0;
}
