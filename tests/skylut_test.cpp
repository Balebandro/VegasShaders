// Real-GPU check of the physical sky's ExactSunlight / MultipleScattering passes (Effects\SkyView.fx.hlsl) without the game: compiles the
// effect file with D3DX (as the game does), draws techniques SkyTrans (1), SkyMS (2) and the sky-view table (0, 3, 4) into the same
// A16B16G16R16F targets, reads them back and writes raw float32 files for bench\deep-roadmap\evidence\skyms\compare_gpu.py; times each
// pass with D3D9 timestamp queries (average of many draws; run this under gpu_locked.sh, it is a benchmark).
// Run from src/hlsl/NewVegas/Effects.
// Usage: skylut_test.exe <output folder> [haze] [albedo]
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* what) {
	if (FAILED(hr)) { char text[256]; sprintf_s(text, "%s failed: %08lx", what, hr); throw std::runtime_error(text); }
}

int main(int argc, char** argv) {
	if (argc < 2) { std::puts("usage: skylut_test <output folder> [haze] [albedo]"); return 2; }
	const std::string out = argv[1];
	const float haze = argc > 2 ? (float)atof(argv[2]) : 1.0f, albedo = argc > 3 ? (float)atof(argv[3]) : 0.3f;
	try {
		HWND window = CreateWindowExA(0, "STATIC", "skylut", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
		ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
		D3DDISPLAYMODE display = {}; d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display);
		D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
		pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = display.Format;
		ComPtr<IDirect3DDevice9> device;
		Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device), "device");
		D3DADAPTER_IDENTIFIER9 id; d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
		D3DCAPS9 caps; device->GetDeviceCaps(&caps);
		std::printf("adapter: %s; ps %lu.%lu; MaxPixelShader30InstructionSlots %lu\n", id.Description, D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion), caps.MaxPixelShader30InstructionSlots);

		ComPtr<ID3DXEffect> effect; ComPtr<ID3DXBuffer> errors;
		D3DXMACRO macros[] = { { "PHYSICAL_SKY_ON", "1" }, { NULL, NULL } };
		HRESULT hr = D3DXCreateEffectFromFileA(device.Get(), "SkyView.fx.hlsl", macros, NULL, 0, NULL, &effect, &errors);
		if (errors) std::puts((const char*)errors->GetBufferPointer());
		Check(hr, "effect");

		auto target = [&](UINT w, UINT h, ComPtr<IDirect3DTexture9>& t, ComPtr<IDirect3DSurface9>& s) {
			Check(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t, NULL), "target");
			Check(t->GetSurfaceLevel(0, &s), "surface");
		};
		ComPtr<IDirect3DTexture9> trans, ms, table; ComPtr<IDirect3DSurface9> transS, msS, tableS;
		target(256, 64, trans, transS); target(32, 32, ms, msS); target(128, 64, table, tableS);

		// the game's frame vertex buffer: a full-screen quad with the half-pixel offset of the SCREEN (2560 x 1440), whatever the target
		struct Vertex { float x, y, z, u, v; };
		const float ox = 0.5f / 2560.0f, oy = 0.5f / 1440.0f;
		const Vertex quad[] = { {-1, 1, 1, ox, oy}, {-1, -1, 1, ox, 1 + oy}, {1, 1, 1, 1 + ox, oy}, {1, -1, 1, 1 + ox, 1 + oy} };
		device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		// the game draws its full-screen passes with these states; the default CCW culling would drop this quad (triangle order of the game's strip)
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device->SetRenderState(D3DRS_ZENABLE, FALSE);
		device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		auto vec = [&](const char* name, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); effect->SetVector(name, &v); };
		auto draw = [&](IDirect3DSurface9* rt, UINT technique) {
			Check(device->SetRenderTarget(0, rt), "SetRenderTarget");
			Check(effect->SetTechnique(effect->GetTechnique(technique)), "SetTechnique");
			// as the game does (EffectRecord::SetCT): the textures go onto the device right after the target is set and before Begin; the effect
			// file gives its samplers no Texture state, so D3DX leaves the stages alone. (D3D9 unbinds a texture while it is the target.)
			device->SetTexture(0, trans.Get());
			device->SetTexture(1, ms.Get());
			UINT passes = 0;
			Check(effect->Begin(&passes, 0), "Begin");
			effect->BeginPass(0);
			Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)), "draw");
			effect->EndPass();
			effect->End();
		};
		auto readback = [&](IDirect3DSurface9* rt, UINT w, UINT h, const std::string& file) {
			ComPtr<IDirect3DSurface9> sys;
			Check(device->CreateOffscreenPlainSurface(w, h, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, NULL), "sysmem");
			Check(device->GetRenderTargetData(rt, sys.Get()), "GetRenderTargetData");
			D3DLOCKED_RECT lr;
			Check(sys->LockRect(&lr, NULL, D3DLOCK_READONLY), "lock");
			std::vector<float> f((size_t)w * h * 4);
			for (UINT y = 0; y < h; y++) D3DXFloat16To32Array(&f[(size_t)y * w * 4], (const D3DXFLOAT16*)((const char*)lr.pBits + (size_t)y * lr.Pitch), w * 4);
			sys->UnlockRect();
			FILE* fp = nullptr;
			fopen_s(&fp, (out + "\\" + file).c_str(), "wb");
			fwrite(f.data(), sizeof(float), f.size(), fp); fclose(fp);
		};

		// ---- the static tables
		vec("TESR_PhysSky", 1.0f, 1.0f, 0.35f, haze);
		vec("TESR_PhysSkySun", 0.0f, 0.0f, 1.0f, albedo);
		device->BeginScene();
		draw(transS.Get(), 1);
		draw(msS.Get(), 2);
		device->EndScene();
		readback(transS.Get(), 256, 64, "trans.bin");
		readback(msS.Get(), 32, 32, "ms.bin");

		// ---- the sky-view table at several sun elevations: closed form (0), exact sunlight (3), with multiple scattering (4)
		const float elev[] = { 90, 45, 15, 5, 3, 0, -2, -4 };
		for (float e : elev) {
			const float z = sinf(e * 3.14159265f / 180.0f);
			vec("TESR_PhysSkySun", cosf(e * 3.14159265f / 180.0f), 0.0f, z, albedo);
			for (UINT tech : { 0u, 3u, 4u }) {
				device->BeginScene();
				draw(tableS.Get(), tech);
				device->EndScene();
				char name[64]; sprintf_s(name, "table_%d_t%u.bin", (int)e, tech);
				readback(tableS.Get(), 128, 64, name);
			}
		}

		// ---- timing: average GPU time of one draw of each pass (timestamp queries around 200 draws, 5 rounds, the fastest round)
		ComPtr<IDirect3DQuery9> disjoint, t0, t1, freq;
		Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint), "disjoint");
		Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0), "ts0");
		Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1), "ts1");
		Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq), "freq");
		UINT64 hz = 0;
		freq->Issue(D3DISSUE_END);
		while (freq->GetData(&hz, sizeof(hz), D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
		struct Pass { const char* name; IDirect3DSurface9* rt; UINT tech; int count; };
		vec("TESR_PhysSkySun", cosf(15.0f * 3.14159265f / 180.0f), 0.0f, sinf(15.0f * 3.14159265f / 180.0f), albedo);
		const Pass passes[] = { { "SkyTrans (256x64, static)", transS.Get(), 1, 20 }, { "SkyMS (32x32, static)", msS.Get(), 2, 20 },
			{ "sky-view table, closed form (today)", tableS.Get(), 0, 400 }, { "sky-view table, exact sunlight", tableS.Get(), 3, 400 }, { "sky-view table, exact + MS", tableS.Get(), 4, 400 } };
		for (const Pass& p : passes) {
			double best = 1e30;
			for (int round = 0; round < 6; round++) {
				device->BeginScene();
				disjoint->Issue(D3DISSUE_BEGIN);
				t0->Issue(D3DISSUE_END);
				for (int i = 0; i < p.count; i++) draw(p.rt, p.tech);
				t1->Issue(D3DISSUE_END);
				disjoint->Issue(D3DISSUE_END);
				device->EndScene();
				UINT64 a = 0, b = 0; BOOL bad = FALSE;
				while (t0->GetData(&a, sizeof(a), D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
				while (t1->GetData(&b, sizeof(b), D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
				while (disjoint->GetData(&bad, sizeof(bad), D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
				if (!bad && round > 0) { const double ms_ = 1000.0 * (double)(b - a) / (double)hz / p.count; if (ms_ < best) best = ms_; }
			}
			std::printf("GPU time per draw  %-40s %.4f ms\n", p.name, best);
		}
		std::puts("done");
	}
	catch (const std::exception& e) { std::printf("FAILED: %s\n", e.what()); return 1; }
	return 0;
}
