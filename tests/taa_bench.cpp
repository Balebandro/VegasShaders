// TAA cost and look, old against new, without the game (2026-10-13): both TAA.fx.hlsl files run on this GPU at 2560 x 1440
// on a detailed test picture with the camera moved a fraction of a pixel sideways (so the history filter really resamples).
//   old: Resolve + full-screen FP16 copy of the resolve into the history buffer + Output (as before)
//   new: Resolve + Output (the two buffers take turns: no copy)
// Prints the GPU time of each and how far the two resolves differ (in 8-bit steps).
// Usage: build\taa-test\taa_bench.exe <old TAA.fx.hlsl> <new TAA.fx.hlsl>
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cmath>
#include <cstdio>
#include <vector>

static const int W = 2560, H = 1440;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAILED %s: 0x%08lX (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static float Hash(int x, int y) { unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (h & 0xFFFF) / 65535.0f; }

int main(int argc, char** argv) {
	if (argc < 3) { std::puts("usage: taa_bench <old TAA.fx.hlsl> <new TAA.fx.hlsl>"); return 2; }
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "TAABench"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("TAABench", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	IDirect3DDevice9* device = nullptr;
	CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));

	// The picture: edges, fine texture and bright spots (HDR), as the frame and as last frame's result.
	std::vector<D3DXFLOAT16> pixels((size_t)W * H * 4);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		const float stripes = ((x / 7 + y / 11) & 1) ? 0.8f : 0.2f;
		const float grain = Hash(x, y);
		const float spot = Hash(x / 3, y / 3) > 0.995f ? 6.0f : 0.0f;
		float rgb[3] = { 0.5f * stripes + 0.4f * grain + spot, 0.4f * stripes + 0.3f * Hash(y, x) + spot, 0.3f * stripes + 0.5f * grain };
		for (int c = 0; c < 3; c++) D3DXFloat32To16Array(&pixels[((size_t)y * W + x) * 4 + c], &rgb[c], 1);
		const float one = 1.0f; D3DXFloat32To16Array(&pixels[((size_t)y * W + x) * 4 + 3], &one, 1);
	}
	auto fp16Texture = [&](IDirect3DTexture9** out, bool target) -> int {
		CHECK(device->CreateTexture(W, H, 1, target ? D3DUSAGE_RENDERTARGET : 0, D3DFMT_A16B16G16R16F, target ? D3DPOOL_DEFAULT : D3DPOOL_SYSTEMMEM, out, NULL));
		return 0;
	};
	IDirect3DTexture9 *upload, *source, *historyA, *historyB, *resolveOld, *resolveNew, *depth, *viewModel, *readback, *readback2;
	if (fp16Texture(&upload, false) || fp16Texture(&source, true) || fp16Texture(&historyA, true) || fp16Texture(&historyB, true) ||
		fp16Texture(&resolveOld, true) || fp16Texture(&resolveNew, true) || fp16Texture(&readback, false) || fp16Texture(&readback2, false)) return 1;
	D3DLOCKED_RECT l;
	CHECK(upload->LockRect(0, &l, NULL, 0));
	for (int y = 0; y < H; y++) memcpy((char*)l.pBits + y * l.Pitch, &pixels[(size_t)y * W * 4], W * 8);
	upload->UnlockRect(0);
	IDirect3DSurface9 *uploadS, *sourceS, *historyAS, *historyBS, *resolveOldS, *resolveNewS, *readbackS, *readback2S;
	upload->GetSurfaceLevel(0, &uploadS); source->GetSurfaceLevel(0, &sourceS); historyA->GetSurfaceLevel(0, &historyAS);
	historyB->GetSurfaceLevel(0, &historyBS); resolveOld->GetSurfaceLevel(0, &resolveOldS); resolveNew->GetSurfaceLevel(0, &resolveNewS);
	readback->GetSurfaceLevel(0, &readbackS); readback2->GetSurfaceLevel(0, &readback2S);
	CHECK(device->UpdateSurface(uploadS, NULL, sourceS, NULL));
	CHECK(device->UpdateSurface(uploadS, NULL, historyAS, NULL));
	CHECK(device->UpdateSurface(uploadS, NULL, historyBS, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &depth, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &viewModel, NULL));
	CHECK(depth->LockRect(0, &l, NULL, 0));
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) ((float*)((char*)l.pBits + y * l.Pitch))[x] = 0.05f + 0.01f * (((x / 64) + (y / 64)) & 1);
	depth->UnlockRect(0);
	CHECK(viewModel->LockRect(0, &l, NULL, 0));
	for (int y = 0; y < H; y++) memset((char*)l.pBits + y * l.Pitch, 0, W * 4);
	viewModel->UnlockRect(0);
	IDirect3DSurface9* back;
	device->GetRenderTarget(0, &back);

	ID3DXEffect* effects[2] = {};
	for (int i = 0; i < 2; i++) {
		ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCreateEffectFromFileA(device, argv[1 + i], NULL, NULL, 0, NULL, &effects[i], &errors))) {
			std::printf("effect %s failed: %s\n", argv[1 + i], errors ? (const char*)errors->GetBufferPointer() : "?");
			return 1;
		}
	}
	// Camera: identity view, 90 degree projection, far 10000; moved 0.15 units sideways since last frame: at depth 500-600
	// that is about a third of a pixel, so the history is read between texels.
	D3DXMATRIX identity, proj;
	D3DXMatrixIdentity(&identity);
	D3DXMatrixIdentity(&proj);
	for (ID3DXEffect* e : effects) {
		auto vec = [&](const char* n, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); e->SetVector(n, &v); };
		vec("TESR_ReciprocalResolution", 1.0f / W, 1.0f / H, 0, 0);
		vec("TESR_TAAData", 0.9f, 1.0f, 1.0f, 0.0f);
		vec("TESR_TAAPrevProjection", 1.0f, 1.0f, 0, 0);
		vec("TESR_TAACameraDelta", 0.15f, 0.07f, 0, 0);
		vec("TESR_TAAWeapon", 1.0f, 0, 0, 0);
		vec("TESR_TAASharpen", 0.5f, 0, 0, 0);
		vec("TESR_CameraData", 5.0f, 10000.0f, 0, 0);
		vec("TESR_DepthConstants", 0, 0, 0, 0);
		e->SetMatrix("TESR_TAAPrevViewTransform", &identity);
		e->SetMatrix("TESR_ViewTransform", &identity);
		e->SetMatrix("TESR_ProjectionTransform", &proj);
	}
	struct V { float x, y, z, w, u, v; };
	const V quad[4] = { { -1 - 1.0f / W, 1 + 1.0f / H, 0, 1, 0, 0 }, { 1 - 1.0f / W, 1 + 1.0f / H, 0, 1, 1, 0 },
		{ -1 - 1.0f / W, -1 + 1.0f / H, 0, 1, 0, 1 }, { 1 - 1.0f / W, -1 + 1.0f / H, 0, 1, 1, 1 } };
	device->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
	auto draw = [&](ID3DXEffect* e, const char* technique) {
		e->SetTechnique(technique);
		UINT n = 0; e->Begin(&n, 0); e->BeginPass(0);
		device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		e->EndPass(); e->End();
	};
	auto bindInputs = [&]() {
		device->SetTexture(0, source); device->SetTexture(2, depth); device->SetTexture(3, viewModel);
	};
	// One frame each way, as TAA.cpp does it.
	auto oldFrame = [&]() {
		bindInputs(); device->SetTexture(1, historyA);
		device->SetRenderTarget(0, resolveOldS); draw(effects[0], "Resolve");
		device->StretchRect(resolveOldS, NULL, historyBS, NULL, D3DTEXF_NONE);	// the copy the new path drops
		device->SetTexture(1, historyB);
		device->SetRenderTarget(0, back); draw(effects[0], "Output");
	};
	auto newFrame = [&]() {
		bindInputs(); device->SetTexture(1, historyA);
		device->SetRenderTarget(0, resolveNewS); draw(effects[1], "Resolve");
		device->SetTexture(1, resolveNew);
		device->SetRenderTarget(0, back); draw(effects[1], "Output");
	};
	// The look: one resolve each from the same history, compared.
	CHECK(device->BeginScene());
	oldFrame(); newFrame();
	CHECK(device->EndScene());
	CHECK(device->GetRenderTargetData(resolveOldS, readbackS));
	CHECK(device->GetRenderTargetData(resolveNewS, readback2S));
	D3DLOCKED_RECT a, b;
	CHECK(readbackS->LockRect(&a, NULL, D3DLOCK_READONLY));
	CHECK(readback2S->LockRect(&b, NULL, D3DLOCK_READONLY));
	double sum = 0.0, worst = 0.0; size_t over = 0, count = 0;
	for (int y = 2; y < H - 2; y++) for (int x = 2; x < W - 2; x++) {
		float pa[4], pb[4];
		D3DXFloat16To32Array(pa, (const D3DXFLOAT16*)((char*)a.pBits + y * a.Pitch) + x * 4, 4);
		D3DXFloat16To32Array(pb, (const D3DXFLOAT16*)((char*)b.pBits + y * b.Pitch) + x * 4, 4);
		for (int c = 0; c < 3; c++) {
			// In display steps: tonemapped (x / (1 + x)), 8 bits.
			const double d = fabs(pa[c] / (1.0 + pa[c]) - pb[c] / (1.0 + pb[c])) * 255.0;
			sum += d; worst = d > worst ? d : worst; over += d > 1.0; count++;
		}
	}
	readbackS->UnlockRect(); readback2S->UnlockRect();
	std::printf("LOOK: new resolve vs old, camera moved a third of a pixel: mean %.3f, worst %.2f 8-bit steps, %.3f%% of values over 1 step\n",
		sum / count, worst, 100.0 * over / count);

	// The cost: GPU timestamps round 300 frames of each, after a warm-up.
	IDirect3DQuery9 *t0, *t1, *disjoint, *frequency;
	device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0); device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1);
	device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint); device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency);
	auto timeIt = [&](auto&& frame, double& ms) -> int {
		for (int i = 0; i < 100; i++) { device->BeginScene(); frame(); device->EndScene(); }
		double total = 0.0; int good = 0;
		for (int i = 0; i < 300; i++) {
			disjoint->Issue(D3DISSUE_BEGIN); t0->Issue(D3DISSUE_END);
			device->BeginScene(); frame(); device->EndScene();
			t1->Issue(D3DISSUE_END); disjoint->Issue(D3DISSUE_END); frequency->Issue(D3DISSUE_END);
			UINT64 s = 0, e = 0, f = 1; BOOL broken = TRUE;
			while (t0->GetData(&s, sizeof(s), D3DGETDATA_FLUSH) == S_FALSE) {}
			while (t1->GetData(&e, sizeof(e), D3DGETDATA_FLUSH) == S_FALSE) {}
			while (disjoint->GetData(&broken, sizeof(broken), D3DGETDATA_FLUSH) == S_FALSE) {}
			while (frequency->GetData(&f, sizeof(f), D3DGETDATA_FLUSH) == S_FALSE) {}
			if (!broken) { total += (double)(e - s) * 1000.0 / f; good++; }
		}
		ms = good ? total / good : -1.0;
		return 0;
	};
	double oldMs = 0, newMs = 0, oldMs2 = 0, newMs2 = 0;
	timeIt(oldFrame, oldMs); timeIt(newFrame, newMs); timeIt(oldFrame, oldMs2); timeIt(newFrame, newMs2);	// twice, interleaved
	std::printf("COST at %dx%d: old %.3f / %.3f ms, new %.3f / %.3f ms (two runs each) -> saves %.3f ms (%.0f%%)\n", W, H,
		oldMs, oldMs2, newMs, newMs2, 0.5 * (oldMs + oldMs2 - newMs - newMs2), 100.0 * (1.0 - (newMs + newMs2) / (oldMs + oldMs2)));
	return 0;
}
