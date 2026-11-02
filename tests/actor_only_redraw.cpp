// RedrawActorsOnly GPU check (ShadowManager::RenderShadowCubeMap, CubeLayer::MovingOnto): on this machine's D3D9 (or a
// DXVK d3d9.dll put next to the exe), can a face of an R32F cube render target be copied into another with StretchRect,
// and does a min blend into it keep the nearer distance? Draws two quads (one nearer, one farther than the copied value)
// over a copied face and reads the face back.
// Build + run: tests\run_actor_only_redraw.ps1 [-Dxvk <folder with a 32-bit d3d9.dll>]
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#pragma comment(lib, "d3d9.lib")

static int failures = 0;
#define CHECK(cond, ...) do { std::printf((cond) ? "PASS: " : "FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); if (!(cond)) failures++; } while (0)

struct Vertex { float x, y, z, rhw; D3DCOLOR colour; };

int main() {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) { std::puts("FAIL: no Direct3D 9"); return 1; }
	HWND window = CreateWindowA("STATIC", "actor_only_redraw", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
	D3DPRESENT_PARAMETERS present = {};
	present.Windowed = TRUE;
	present.SwapEffect = D3DSWAPEFFECT_DISCARD;
	present.BackBufferFormat = D3DFMT_UNKNOWN;
	present.hDeviceWindow = window;
	IDirect3DDevice9* device = nullptr;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device))) {
		std::puts("FAIL: no device"); return 1;
	}
	D3DADAPTER_IDENTIFIER9 id = {};
	d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
	std::printf("GPU: %s\n", id.Description);

	// What ActorsOnlySupported asks.
	D3DCAPS9 caps = {};
	device->GetDeviceCaps(&caps);
	D3DDISPLAYMODE mode = {};
	device->GetDisplayMode(0, &mode);
	const bool blendOp = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) != 0;
	const bool blendR32F = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
		D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_CUBETEXTURE, D3DFMT_R32F));
	CHECK(blendOp, "the device has blend operations (min)");
	CHECK(blendR32F, "R32F cube render targets can be blended");

	const UINT size = 64;
	IDirect3DCubeTexture9 *staticCube = nullptr, *liveCube = nullptr;
	if (FAILED(device->CreateCubeTexture(size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &staticCube, NULL)) ||
		FAILED(device->CreateCubeTexture(size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &liveCube, NULL))) {
		std::puts("FAIL: R32F cube render targets"); return 1;
	}
	IDirect3DSurface9 *staticFace = nullptr, *liveFace = nullptr, *readback = nullptr, *depth = nullptr;
	staticCube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_Z, 0, &staticFace);
	liveCube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_Z, 0, &liveFace);
	device->CreateOffscreenPlainSurface(size, size, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &readback, NULL);
	device->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, NULL);

	// The static layer: 0.5 everywhere (a wall at half the lamp's radius); the live face starts as garbage (1.0).
	device->SetRenderTarget(0, staticFace);
	device->SetDepthStencilSurface(depth);
	device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 128, 0, 0), 1.0f, 0);
	device->SetRenderTarget(0, liveFace);
	device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 255, 0, 0), 1.0f, 0);

	const HRESULT copied = device->StretchRect(staticFace, NULL, liveFace, NULL, D3DTEXF_NONE);
	CHECK(SUCCEEDED(copied), "StretchRect copies one R32F cube face into another (hr %08lx)", (unsigned long)copied);

	// The moving layer: a person nearer than the wall on the left half (0.3), one farther on the right half (0.8).
	device->SetRenderTarget(0, liveFace);
	device->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
	device->SetRenderState(D3DRS_LIGHTING, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MIN);
	device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
	device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
	device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	const float h = (float)size, half = h / 2;
	const D3DCOLOR nearer = D3DCOLOR_ARGB(255, 77, 0, 0), farther = D3DCOLOR_ARGB(255, 204, 0, 0);
	const Vertex left[4] = { { -0.5f, -0.5f, 0.5f, 1, nearer }, { half - 0.5f, -0.5f, 0.5f, 1, nearer }, { -0.5f, h - 0.5f, 0.5f, 1, nearer }, { half - 0.5f, h - 0.5f, 0.5f, 1, nearer } };
	const Vertex right[4] = { { half - 0.5f, -0.5f, 0.5f, 1, farther }, { h - 0.5f, -0.5f, 0.5f, 1, farther }, { half - 0.5f, h - 0.5f, 0.5f, 1, farther }, { h - 0.5f, h - 0.5f, 0.5f, 1, farther } };
	device->BeginScene();
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, left, sizeof(Vertex));
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, right, sizeof(Vertex));
	device->EndScene();

	const HRESULT read = device->GetRenderTargetData(liveFace, readback);
	CHECK(SUCCEEDED(read), "the live face reads back");
	D3DLOCKED_RECT lock = {};
	if (SUCCEEDED(read) && SUCCEEDED(readback->LockRect(&lock, NULL, D3DLOCK_READONLY))) {
		const float* row = (const float*)((const char*)lock.pBits + lock.Pitch * (size / 2));
		const float l = row[size / 4], r = row[3 * size / 4];
		readback->UnlockRect();
		std::printf("left (person nearer than the wall) %.3f, right (person behind the wall) %.3f\n", l, r);
		CHECK(fabsf(l - 77 / 255.0f) < 0.01f, "the nearer person's distance wins over the copied wall (%.3f)", l);
		CHECK(fabsf(r - 128 / 255.0f) < 0.01f, "the copied wall's distance stays where the person is behind it (%.3f)", r);
	}
	std::printf(failures ? "%d check(s) FAILED\n" : "All RedrawActorsOnly GPU checks passed\n", failures);
	return failures ? 1 : 0;
}
