// UNOFFICIAL 2026-10-29: Includes/LightCommon.hlsl LightCommonIndex on the GPU. Packs lamp indices as core/ManyLamps.h does (two 12-bit
// indices a float, eight a float4, three float4), decodes lamp k = 0..23 in a pixel shader (pixel k of a 24 x 1 target, R32F), reads the
// target back and compares. Several index sets, including 0, 4095 (no lamp) and large ones. Exit code 1 on any mismatch.
// Usage: lightcommon_index_test.exe <path to src\hlsl\NewVegas\Shaders\Includes\LightCommon.hlsl>
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %s (0x%08lX)\n", #x, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char** argv) {
	if (argc < 2) { printf("usage: lightcommon_index_test <LightCommon.hlsl>\n"); return 2; }
	std::ifstream in(argv[1]); std::stringstream ss; ss << in.rdbuf();
	const std::string src = ss.str() +
		"float4 P[3] : register(c0);\n"
		"float4 main(float2 vpos : VPOS) : COLOR0 { return LightCommonIndex(P[0], P[1], P[2], floor(vpos.x)).xxxx; }\n";
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	UINT adapter = D3DADAPTER_DEFAULT;
	for (UINT a = 0; a < d3d->GetAdapterCount(); a++) { D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(a, 0, &id); if (strstr(id.Description, "NVIDIA")) adapter = a; }
	D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(adapter, 0, &id); printf("GPU: %s\n", id.Description);
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "lcidx"; wc.hInstance = GetModuleHandleA(NULL); RegisterClassA(&wc);
	HWND window = CreateWindowA("lcidx", "lcidx", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
	IDirect3DDevice9* d = nullptr;
	CHECK(d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d));
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCompileShader(src.c_str(), (UINT)src.size(), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL))) {
		printf("compile failed: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?"); return 1;
	}
	IDirect3DPixelShader9* ps = nullptr; CHECK(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps));
	IDirect3DSurface9 *rt = nullptr, *copy = nullptr;
	CHECK(d->CreateRenderTarget(24, 1, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
	CHECK(d->CreateOffscreenPlainSurface(24, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &copy, NULL));
	struct V { float x, y, z, w; };
	const V quad[4] = { { -0.5f, -0.5f, 0, 1 }, { 23.5f, -0.5f, 0, 1 }, { -0.5f, 0.5f, 0, 1 }, { 23.5f, 0.5f, 0, 1 } };
	int failures = 0;
	for (int set = 0; set < 6; set++) {
		int idx[24];
		for (int k = 0; k < 24; k++) idx[k] = set == 0 ? k : set == 1 ? 4095 - k : set == 2 ? (k * 389 + 7) % 4096 : set == 3 ? 4095 : set == 4 ? 0 : (k % 3 ? 1023 : 446);
		float packed[12];
		for (int f = 0; f < 12; f++) packed[f] = (float)(idx[f * 2] + 4096 * idx[f * 2 + 1]);	// as ManyLamps::SetForPassTable
		d->SetRenderTarget(0, rt);
		d->SetPixelShader(ps);
		d->SetPixelShaderConstantF(0, packed, 3);
		d->SetFVF(D3DFVF_XYZRHW);
		d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->BeginScene(); d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)); d->EndScene();
		CHECK(d->GetRenderTargetData(rt, copy));
		D3DLOCKED_RECT r; CHECK(copy->LockRect(&r, NULL, D3DLOCK_READONLY));
		const float* got = (const float*)r.pBits;
		for (int k = 0; k < 24; k++) if (got[k] != (float)idx[k]) { if (failures++ < 12) printf("set %d lamp %d: want %d, got %.3f\n", set, k, idx[k], got[k]); }
		copy->UnlockRect();
	}
	printf(failures ? "FAIL: %d wrong\n" : "PASS: every lamp index decoded exactly (6 sets x 24)\n", failures);
	return failures ? 1 : 0;
}
