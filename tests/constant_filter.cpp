// UNOFFICIAL: device test for src/core/ConstantFilter.h (SkipRedundantConstants). Real D3D9 device, the filter's own
// body detours. After every operation the device's float constants (read back) must equal what the program last set,
// including after D3DX effects restore their saved state at End (IDirect3DStateBlock9::Apply), state block recording and
// explicit state block Apply; and redundant calls must actually be skipped.
// Usage: build\constant-filter-test\constant_filter.exe
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <thread>
#include "../lib/Detours/detours.h"

// Stand-ins for what the header takes from NVR.
struct GpuTimer { static inline bool Enabled = true; };
struct Logger { static void Log(const char* format, ...) { va_list a; va_start(a, format); vprintf(format, a); va_end(a); std::puts(""); } };
struct SettingStub { int GetSettingI(const char*, const char*) { return getenv("FILTER") && !strcmp(getenv("FILTER"), "0") ? 0 : 1; } };
static SettingStub settingStub;
static SettingStub* TheSettingManager = &settingStub;
#include "../src/core/ConstantFilter.h"

static unsigned seed = 7;
static float Rnd() { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 9) % 8) * 0.25f; }   // few distinct values: many repeats

static const char* EffectSource = R"(
float4 A : register(c10);
float4 B[4] : register(c20);
float4 PS() : COLOR0 { return A + B[0] + B[1] + B[2] + B[3]; }
technique T { pass P { PixelShader = compile ps_2_0 PS(); } }
)";

int main() {
	HWND window = CreateWindowExA(0, "STATIC", "constant filter test", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DDISPLAYMODE mode; d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
	pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = mode.Format;
	IDirect3DDevice9* device = nullptr;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) { std::puts("no device"); return 1; }
	ConstantFilter::BeginFrame(device);

	ID3DXEffect* effect = nullptr; ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCreateEffect(device, EffectSource, (UINT)strlen(EffectSource), NULL, NULL, 0, NULL, &effect, &errors))) { std::puts("effect failed"); return 1; }

	float expected[224][4] = {};
	for (int r = 0; r < 224; r++) for (int k = 0; k < 4; k++) expected[r][k] = 0;
	device->SetPixelShaderConstantF(0, &expected[0][0], 224);
	int failures = 0, checks = 0;
	auto check = [&](const char* what) {
		float actual[224][4];
		device->GetPixelShaderConstantF(0, &actual[0][0], 224);
		checks++;
		for (int r = 0; r < 100; r++)   // the registers this test sets (D3DX keeps its own at c128+)
			if (memcmp(actual[r], expected[r], 16)) { printf("FAIL after %s: c%d is (%g %g %g %g), expected (%g %g %g %g)\n", what, r,
				actual[r][0], actual[r][1], actual[r][2], actual[r][3], expected[r][0], expected[r][1], expected[r][2], expected[r][3]); failures++; return; }
	};
	auto set = [&](UINT start, UINT count) {
		std::vector<float> v(count * 4);
		for (float& x : v) x = Rnd();
		device->SetPixelShaderConstantF(start, v.data(), count);
		memcpy(expected[start], v.data(), v.size() * sizeof(float));
	};

	for (int round = 0; round < 2000; round++) {
		const int op = round % 7;
		if (op <= 2) { set((UINT)(Rnd() * 40), 1 + (UINT)(Rnd() * 4)); check("a set"); }
		else if (op == 3) {
			// An effect sets c10 and c20-23 between Begin and End; End restores them (its saved state block).
			UINT passes = 0;
			const D3DXVECTOR4 value(Rnd(), Rnd(), Rnd(), Rnd());
			effect->SetVector("A", &value);
			effect->Begin(&passes, 0); effect->BeginPass(0); effect->EndPass(); effect->End();
			check("an effect's End");
			// Now set what the effect had set: must reach the device (the copy was wiped by Apply).
			D3DXVECTOR4 a; effect->GetVector("A", &a);
			device->SetPixelShaderConstantF(10, (float*)&a, 1); memcpy(expected[10], &a, 16);
			check("setting the effect's value after End");
		}
		else if (op == 4) {
			// Recorded, not applied; then applied later by Apply.
			device->BeginStateBlock();
			float v[4] = { Rnd(), Rnd(), Rnd(), 9.0f };
			device->SetPixelShaderConstantF(5, v, 1);
			IDirect3DStateBlock9* block = nullptr;
			device->EndStateBlock(&block);
			check("recording a state block");
			if (block) { block->Apply(); memcpy(expected[5], v, 16); block->Release(); }
			check("applying a recorded state block");
		}
		else if (op == 5) {
			// Capture now, change, apply the capture: the device goes back without our calls.
			IDirect3DStateBlock9* block = nullptr;
			device->CreateStateBlock(D3DSBT_PIXELSTATE, &block);
			float saved[4]; memcpy(saved, expected[7], 16);
			set(7, 1);
			if (block) { block->Apply(); memcpy(expected[7], saved, 16); block->Release(); }
			check("applying a captured state block");
			device->SetPixelShaderConstantF(7, expected[7], 1); check("re-setting after Apply");
			// The sharp case: set a value, let a state block put the old one back behind the filter's back, then set the
			// same value again. A stale copy would think the device still holds it and drop the call.
			IDirect3DStateBlock9* undo = nullptr;
			device->CreateStateBlock(D3DSBT_PIXELSTATE, &undo);
			float fresh[4] = { Rnd(), Rnd(), Rnd(), 5.0f };
			device->SetPixelShaderConstantF(8, fresh, 1);
			if (undo) { undo->Apply(); undo->Release(); }
			device->SetPixelShaderConstantF(8, fresh, 1); memcpy(expected[8], fresh, 16);
			check("setting a value again after a state block undid it");
		}
		else { set(0, 1); set(0, 1); check("repeated sets"); }
		if (round % 97 == 50) {
			// Another thread (the game's loading screen) changes a register behind the frame thread's back; setting the old
			// value again must reach the device, and filtering must come back after 30 quiet frames.
			float before[4]; memcpy(before, expected[9], 16);
			device->SetPixelShaderConstantF(9, before, 1);
			float other[4] = { 7.0f, 7.0f, 7.0f, (float)round };
			std::thread([&] { device->SetPixelShaderConstantF(9, other, 1); }).join();
			memcpy(expected[9], other, 16);
			check("another thread's set");
			device->SetPixelShaderConstantF(9, before, 1); memcpy(expected[9], before, 16);
			check("setting the old value again after another thread changed it");
			const double skippedBefore = ConstantFilter::StatPixelSkipped;
			for (int f = 0; f < 31; f++) ConstantFilter::BeginFrame(device);
			if (ConstantFilter::Foreign) { std::puts("FAIL: still paused after 31 quiet frames"); failures++; }
			device->SetPixelShaderConstantF(9, before, 1); device->SetPixelShaderConstantF(9, before, 1);
			check("after the pause");
			if (ConstantFilter::StatPixelSkipped == skippedBefore && !(getenv("FILTER") && !strcmp(getenv("FILTER"), "0"))) {
				std::puts("FAIL: not skipping again after the pause"); failures++;
			}
		}
	}
	printf("CONSTANT FILTER test: %d checks, %d failures | pixel calls %.0f, skipped %.0f (%.0f%%), copy wiped %.0f times\n", checks, failures,
		ConstantFilter::StatPixelCalls, ConstantFilter::StatPixelSkipped, 100.0 * ConstantFilter::StatPixelSkipped / std::max(ConstantFilter::StatPixelCalls, 1.0),
		ConstantFilter::StatWipes);
	if (ConstantFilter::StatPixelSkipped == 0) { std::puts("FAIL: nothing was skipped"); failures++; }
	effect->Release(); device->Release(); d3d->Release();
	std::puts(failures ? "FAILED" : "All checks passed.");
	return failures ? 1 : 0;
}
