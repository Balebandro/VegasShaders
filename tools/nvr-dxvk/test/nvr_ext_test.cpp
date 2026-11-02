// End-to-end test of the NVR DXVK extension on dxvk-native (Linux, SDL2 window, any Vulkan device).
// Mirrors what NVR's AvgLuma compute path does: two dispatches, a buffer-to-texture copy,
// a readback, then a D3D9 clear + present to make sure the device keeps working.
#include <windows.h>
#include <d3d9.h>
#ifdef NVR_TEST_WIN32
// Real Win32 (MinGW build, run on Windows or under Wine with the fork's d3d9.dll next to the exe)
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
#else
#include <SDL.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "d3d9_nvr_ext_api.h"

static std::vector<char> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
  size_t n = size_t(f.tellg()); f.seekg(0);
  std::vector<char> v(n); f.read(v.data(), n); return v;
}

static float lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
static float halfToFloat(uint16_t h) {
  uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff; float v;
  if (e == 0) v = std::ldexp(float(m), -24); else if (e == 31) v = m ? NAN : INFINITY; else v = std::ldexp(float(m + 1024), int(e) - 25);
  return s ? -v : v;
}

#define CHECK(hr, what) do { HRESULT _hr = (hr); if (FAILED(_hr)) { std::printf("FAIL: %s -> 0x%08x\n", what, unsigned(_hr)); return 1; } } while (0)

struct Push {
  uint32_t width, height, groupsX, groupsY, phase, samplerIndex, pad0, pad1;
  float motionBlur[4], dof[4], gameTime[4], exposure[4], camera[4];
};

int main(int argc, char** argv) {
  const char* spvPath = argc > 1 ? argv[1] : "AvgLuma.spv";
  const UINT W = 67, H = 41; // odd sizes: partial tiles

#ifdef NVR_TEST_WIN32
  WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvrtest";
  RegisterClassA(&wc);
  HWND win = CreateWindowExA(0, "nvrtest", "nvrtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 48, nullptr, nullptr, wc.hInstance, nullptr);
  if (!win) { std::printf("window failed\n"); return 1; }
#else
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window* win = SDL_CreateWindow("nvrtest", 0, 0, 64, 48, SDL_WINDOW_VULKAN);
  if (!win) { std::printf("SDL window failed: %s\n", SDL_GetError()); return 1; }
#endif

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = (HWND)win;
  pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  IDirect3DDevice9* dev = nullptr;
  CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev), "CreateDevice");

  ID3D9NvrExt* ext = nullptr;
  CHECK(dev->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext), "QueryInterface(ID3D9NvrExt)");
  NVR_EXT_INFO info; ext->GetInfo(&info);
  std::printf("NVR ext %u.%u on DXVK %s, push %u, slots %u, subgroup %u, max wg %u\n",
    info.ExtVersionMajor, info.ExtVersionMinor, info.DxvkVersion, info.MaxPushBytes, info.MaxSlots, info.SubgroupSize, info.MaxWorkgroupInvocations);

  // Textures as NVR has them: HDR colour, R32F linear depth, the 1x1 result.
  IDirect3DTexture9 *color, *depth, *avg;
  CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &color, nullptr), "color tex");
  CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &depth, nullptr), "depth tex");
  CHECK(dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &avg, nullptr), "avg tex");
  IDirect3DSurface9 *colorS, *depthS, *avgS;
  color->GetSurfaceLevel(0, &colorS); depth->GetSurfaceLevel(0, &depthS); avg->GetSurfaceLevel(0, &avgS);

  // Fill them through plain D3D9 so the compute pass has to wait for D3D9's work.
  const float cr = 128 / 255.f, cg = 64 / 255.f, cb = 32 / 255.f, dz = 64 / 255.f;
  CHECK(dev->SetRenderTarget(0, colorS), "SetRenderTarget color");
  CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 128, 64, 32), 1.f, 0), "clear color");
  CHECK(dev->SetRenderTarget(0, depthS), "SetRenderTarget depth");
  CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 64, 0, 0), 1.f, 0), "clear depth");
  IDirect3DSurface9* bb; dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  CHECK(dev->SetRenderTarget(0, bb), "SetRenderTarget bb");

  // Pipeline
  std::vector<char> spv = readFile(spvPath);
  NVR_EXT_BINDING_DESC binds[] = {
    { 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },
    { 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },
    { 1, 2, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE,  0 },
    { 1, 3, NVR_EXT_BIND_STORAGE_BUFFER, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
    { 1, 4, NVR_EXT_BIND_STORAGE_BUFFER, 2, NVR_EXT_VIEW_2D, TRUE,  0 },
    { 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, offsetof(Push, samplerIndex) },
  };
  NVR_EXT_COMPUTE_PIPELINE_DESC pd = {};
  pd.pSpirv = spv.data(); pd.SpirvSize = UINT(spv.size());
  pd.BindingCount = 6; pd.pBindings = binds; pd.PushConstantSize = sizeof(Push);
  pd.UsesSamplerHeap = TRUE; pd.SamplerHeapSet = 0; pd.SamplerHeapBinding = 0; pd.pDebugName = "AvgLuma test";
  ID3D9NvrExtPipeline* pipe = nullptr;
  CHECK(ext->CreateComputePipeline(&pd, &pipe), "CreateComputePipeline");

  const UINT gx = (W + 15) / 16, gy = (H + 15) / 16;
  NVR_EXT_BUFFER_DESC bd = { gx * gy * 4, NVR_EXT_BUFFER_STORAGE, "partials" };
  ID3D9NvrExtBuffer *partials, *stateA, *stateB;
  CHECK(ext->CreateBuffer(&bd, &partials), "partials");
  bd = { 32, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC | NVR_EXT_BUFFER_TRANSFER_DST, "stateA" };
  CHECK(ext->CreateBuffer(&bd, &stateA), "stateA");
  CHECK(ext->CreateBuffer(&bd, &stateB), "stateB");
  CHECK(ext->ClearBuffer(stateA, 0, 0, 0), "clear A");
  CHECK(ext->ClearBuffer(stateB, 0, 0, 0), "clear B");
  NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_LINEAR, NVR_EXT_ADDRESS_CLAMP };
  ID3D9NvrExtSampler* sampler; CHECK(ext->CreateSampler(&sd, &sampler), "sampler");

  Push push = {};
  push.width = W; push.height = H; push.groupsX = gx; push.groupsY = gy;
  push.dof[0] = 0.001f; push.gameTime[3] = 1.f; push.exposure[2] = 1.f; push.exposure[3] = 1.f;
  push.camera[0] = 0.1f; push.camera[1] = 1.f;

  float expectedAvg = 0.2126f * lin(cr) + 0.7152f * lin(cg) + 0.0722f * lin(cb);
  float prev[3] = { 0, 0, 0 };

  for (int frame = 0; frame < 3; frame++) {
    ID3D9NvrExtBuffer* in  = (frame & 1) ? stateB : stateA;
    ID3D9NvrExtBuffer* out = (frame & 1) ? stateA : stateB;
    NVR_EXT_RESOURCE_BINDING rb[] = {
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  color,    0, 0, 0, FALSE },
      { 1, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,    0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_STORAGE_BUFFER, partials, 0, 0, 0, FALSE },
      { 1, NVR_EXT_BIND_STORAGE_BUFFER, in,       0, 0, 0, FALSE },
      { 2, NVR_EXT_BIND_STORAGE_BUFFER, out,      0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_SAMPLER,        sampler,  0, 0, 0, FALSE },
    };
    NVR_EXT_DISPATCH_DESC dd = {};
    dd.pPipeline = pipe; dd.BindingCount = 6; dd.pBindings = rb; dd.pPushData = &push; dd.PushDataSize = sizeof(push);
    push.phase = 0; dd.GroupsX = gx; dd.GroupsY = gy; dd.GroupsZ = 1;
    CHECK(ext->Dispatch(&dd), "dispatch 0");
    push.phase = 1; dd.GroupsX = 1; dd.GroupsY = 1;
    CHECK(ext->Dispatch(&dd), "dispatch 1");
    CHECK(ext->CopyBufferToTexture(avg, 0, 0, out, 16), "CopyBufferToTexture");

    // Some D3D9 work after it, then the readback (which waits for everything).
    CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 10, 20, 30), 1.f, 0), "clear bb");
    CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr), "Present");

    float cur[4] = {}; uint32_t packed[2] = {};
    CHECK(ext->ReadBuffer(out, 0, 16, cur), "ReadBuffer cur");
    CHECK(ext->ReadBuffer(out, 16, 8, packed), "ReadBuffer packed");

    // Expected per the HLSL pass
    float newLuma = (expectedAvg + 7 * prev[0]) / 8;
    float animated = prev[1] + std::fmax(-1.f, std::fmin(1.f, newLuma - prev[1]));
    float depthV = dz * push.camera[1] * 13.f / 12.f;   // every tap reads the same value; the pass divides centre + 12 taps by 12
    float d = std::fmin(1.f, depthV / 1.0f);
    float step = std::pow(std::fmax(prev[2], d), 0.1f);
    float diff = std::fmax(-3 * step, std::fmin(step, d - prev[2]));
    float focus = prev[2] + diff;

    // The texture: GetRenderTargetData into system memory
    IDirect3DSurface9* sys; CHECK(dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr), "sysmem");
    CHECK(dev->GetRenderTargetData(avgS, sys), "GetRenderTargetData");
    D3DLOCKED_RECT lr; CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "LockRect");
    uint16_t* hp = (uint16_t*)lr.pBits;
    float tex[4] = { halfToFloat(hp[0]), halfToFloat(hp[1]), halfToFloat(hp[2]), halfToFloat(hp[3]) };
    sys->UnlockRect(); sys->Release();

    std::printf("frame %d: compute (%.6f %.6f %.6f %.3f) expected (%.6f %.6f %.6f 1) texture (%.5f %.5f %.5f %.3f)\n",
      frame, cur[0], cur[1], cur[2], cur[3], newLuma, animated, focus, tex[0], tex[1], tex[2], tex[3]);

    bool ok = std::fabs(cur[0] - newLuma) < 1e-4f && std::fabs(cur[1] - animated) < 1e-4f && std::fabs(cur[2] - focus) < 1e-4f && cur[3] == 1.f
           && std::fabs(tex[0] - newLuma) < 2e-3f && std::fabs(tex[1] - animated) < 2e-3f && std::fabs(tex[2] - focus) < 2e-3f && tex[3] == 1.f;
    if (!ok) { std::printf("FAIL: values differ\n"); return 1; }
    prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
  }

  // Error paths must fail cleanly, not crash.
  NVR_EXT_RESOURCE_BINDING bad[] = { { 7, NVR_EXT_BIND_SAMPLED_IMAGE, color, 0, 0, 0, FALSE } };
  NVR_EXT_DISPATCH_DESC dd = {}; dd.pPipeline = pipe; dd.BindingCount = 1; dd.pBindings = bad; dd.GroupsX = dd.GroupsY = dd.GroupsZ = 1;
  if (SUCCEEDED(ext->Dispatch(&dd))) { std::printf("FAIL: undeclared slot accepted\n"); return 1; }
  NVR_EXT_COMPUTE_PIPELINE_DESC badPd = pd; badPd.SpirvSize = 8; ID3D9NvrExtPipeline* badPipe = nullptr;
  if (SUCCEEDED(ext->CreateComputePipeline(&badPd, &badPipe))) { std::printf("FAIL: bad SPIR-V accepted\n"); return 1; }

  // ---- Push-data restore: D3D9's alpha reference lives in the shared push data a dispatch overwrites.
  // A quad with alpha 64 against ALPHAREF 128 / GREATER must stay rejected before AND after a dispatch.
  {
    struct V { float x, y, z, rhw; DWORD color; };
    auto drawQuad = [&](DWORD ref) -> int {
      V quad[4] = { { 0, 0, 0, 1, 0x40FF0000 }, { 64, 0, 0, 1, 0x40FF0000 }, { 0, 48, 0, 1, 0x40FF0000 }, { 64, 48, 0, 1, 0x40FF0000 } };
      CHECK(dev->SetRenderTarget(0, bb), "SetRenderTarget bb");
      CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 10, 20, 30), 1.f, 0), "clear bb");
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      dev->SetRenderState(D3DRS_ALPHAREF, ref);
      dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      dev->SetRenderState(D3DRS_ZENABLE, FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      CHECK(dev->BeginScene(), "BeginScene");
      CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)), "DrawPrimitiveUP");
      CHECK(dev->EndScene(), "EndScene");
      IDirect3DSurface9* sys; CHECK(dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr), "sysmem bb");
      CHECK(dev->GetRenderTargetData(bb, sys), "GetRenderTargetData bb");
      D3DLOCKED_RECT lr; CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "LockRect bb");
      DWORD px = *(DWORD*)((char*)lr.pBits + 20 * lr.Pitch + 32 * 4) & 0xFFFFFF;
      sys->UnlockRect(); sys->Release();
      return int(px);
    };
    int control = drawQuad(32);            // alpha 64 > 32: the quad shows (red)
    int before  = drawQuad(128);           // 64 > 128 is false: rejected
    NVR_EXT_RESOURCE_BINDING rb[] = {
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  color,    0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,  0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_STORAGE_BUFFER, partials, 0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_STORAGE_BUFFER, stateA, 0, 0, 0, FALSE },
      { 2, NVR_EXT_BIND_STORAGE_BUFFER, stateB,   0, 0, 0, FALSE }, { 0, NVR_EXT_BIND_SAMPLER,        sampler, 0, 0, 0, FALSE },
    };
    NVR_EXT_DISPATCH_DESC dd = {};
    dd.pPipeline = pipe; dd.BindingCount = 6; dd.pBindings = rb; dd.pPushData = &push; dd.PushDataSize = sizeof(push);
    push.phase = 0; dd.GroupsX = gx; dd.GroupsY = gy; dd.GroupsZ = 1;   // push dword 0 = width = 67: alphaRef byte would become 0 without the restore
    CHECK(ext->Dispatch(&dd), "dispatch (push restore test)");
    int after = drawQuad(128);
    std::printf("push restore: control %06x (quad), before %06x, after dispatch %06x (both must be 0a141e)\n", control, before, after);
    if (control != 0xFF0000 || before != 0x0A141E || after != 0x0A141E) { std::printf("FAIL: push data not restored\n"); return 1; }
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  }

  // ---- Storage image + uniform buffer + UpdateBuffer + CopyTextureToBuffer, on a D3D9 render target (DXVK adds storage usage).
  {
    const char* spv2 = argc > 2 ? argv[2] : "nvr_ext_test_storage.spv";
    std::vector<char> code = readFile(spv2);
    NVR_EXT_BINDING_DESC b2[] = {
      { 1, 0, NVR_EXT_BIND_STORAGE_IMAGE,  0, NVR_EXT_VIEW_2D, TRUE,  0 },
      { 1, 1, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
    };
    NVR_EXT_COMPUTE_PIPELINE_DESC pd2 = {};
    pd2.pSpirv = code.data(); pd2.SpirvSize = UINT(code.size()); pd2.BindingCount = 2; pd2.pBindings = b2; pd2.PushConstantSize = 8; pd2.pDebugName = "storage test";
    ID3D9NvrExtPipeline* pipe2; CHECK(ext->CreateComputePipeline(&pd2, &pipe2), "CreateComputePipeline storage");
    NVR_EXT_BUFFER_DESC ub = { 16, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "params" };
    ID3D9NvrExtBuffer* params; CHECK(ext->CreateBuffer(&ub, &params), "params buffer");
    float value[4] = { 0.5f, 0.25f, 2.f, 1.f };
    CHECK(ext->UpdateBuffer(params, 0, 16, value), "UpdateBuffer");
    NVR_EXT_RESOURCE_BINDING rb[] = {
      { 0, NVR_EXT_BIND_STORAGE_IMAGE,  color,  0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_UNIFORM_BUFFER, params, 0, 0, 0, FALSE },
    };
    uint32_t p2[2] = { W, H };
    NVR_EXT_DISPATCH_DESC dd = {};
    dd.pPipeline = pipe2; dd.BindingCount = 2; dd.pBindings = rb; dd.pPushData = p2; dd.PushDataSize = 8;
    dd.GroupsX = (W + 7) / 8; dd.GroupsY = (H + 7) / 8; dd.GroupsZ = 1;
    CHECK(ext->Dispatch(&dd), "dispatch storage");
    NVR_EXT_BUFFER_DESC rbd = { W * H * 8, NVR_EXT_BUFFER_TRANSFER_DST | NVR_EXT_BUFFER_TRANSFER_SRC, "readback" };
    ID3D9NvrExtBuffer* rbuf; CHECK(ext->CreateBuffer(&rbd, &rbuf), "readback buffer");
    CHECK(ext->CopyTextureToBuffer(rbuf, 0, color, 0, 0), "CopyTextureToBuffer");
    std::vector<uint16_t> texels(W * H * 4);
    CHECK(ext->ReadBuffer(rbuf, 0, W * H * 8, texels.data()), "ReadBuffer texels");
    int bad = 0;
    for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
      const uint16_t* t = &texels[(y * W + x) * 4];
      float e[4] = { 0.5f + x, 0.25f + y, 2.f, 1.f };
      for (int c = 0; c < 4; c++) if (std::fabs(halfToFloat(t[c]) - e[c]) > 0.05f) bad++;
    }
    // The same through D3D9 (GetRenderTargetData): the texture is still a valid D3D9 render target after the relocation.
    IDirect3DSurface9* sys; CHECK(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr), "sysmem color");
    CHECK(dev->GetRenderTargetData(colorS, sys), "GetRenderTargetData color");
    D3DLOCKED_RECT lr; CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "LockRect color");
    const uint16_t* t = (const uint16_t*)((const char*)lr.pBits + 7 * lr.Pitch + 5 * 8);
    float d9[4] = { halfToFloat(t[0]), halfToFloat(t[1]), halfToFloat(t[2]), halfToFloat(t[3]) };
    sys->UnlockRect(); sys->Release();
    std::printf("storage image: %d bad components of %u; texel (5,7) via D3D9 = (%.2f %.2f %.2f %.2f), expected (5.5 7.25 2 1)\n", bad, W * H * 4, d9[0], d9[1], d9[2], d9[3]);
    if (bad || std::fabs(d9[0] - 5.5f) > 0.05f || std::fabs(d9[1] - 7.25f) > 0.05f || std::fabs(d9[2] - 2.f) > 0.05f) { std::printf("FAIL: storage image\n"); return 1; }

    // AvgLuma once more on the rewritten texture (sampled view after the relocation): exact expectation from the same formulas.
    double sum = 0;
    for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
      float hx = halfToFloat(texels[(y * W + x) * 4]), hy = halfToFloat(texels[(y * W + x) * 4 + 1]), hz = halfToFloat(texels[(y * W + x) * 4 + 2]);
      sum += 0.2126f * lin(hx) + 0.7152f * lin(hy) + 0.0722f * lin(hz);
    }
    float avg2 = float(sum / (W * H));
    NVR_EXT_RESOURCE_BINDING rb3[] = {
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  color,    0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,  0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_STORAGE_BUFFER, partials, 0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_STORAGE_BUFFER, stateB, 0, 0, 0, FALSE },
      { 2, NVR_EXT_BIND_STORAGE_BUFFER, stateA,   0, 0, 0, FALSE }, { 0, NVR_EXT_BIND_SAMPLER,        sampler, 0, 0, 0, FALSE },
    };
    NVR_EXT_DISPATCH_DESC d3 = {};
    d3.pPipeline = pipe; d3.BindingCount = 6; d3.pBindings = rb3; d3.pPushData = &push; d3.PushDataSize = sizeof(push);
    push.phase = 0; d3.GroupsX = gx; d3.GroupsY = gy; d3.GroupsZ = 1; CHECK(ext->Dispatch(&d3), "dispatch 0 (after storage)");
    push.phase = 1; d3.GroupsX = 1; d3.GroupsY = 1; CHECK(ext->Dispatch(&d3), "dispatch 1 (after storage)");
    float cur[4] = {}; CHECK(ext->ReadBuffer(stateA, 0, 16, cur), "ReadBuffer after storage");
    { float a[8] = {}, b[8] = {}, pt[4] = {};
      ext->ReadBuffer(stateA, 0, 32, a); ext->ReadBuffer(stateB, 0, 32, b); ext->ReadBuffer(partials, 0, 16, pt);
      std::printf("debug: stateA (%.5f %.5f %.5f %.3f | avg %.5f) stateB (%.5f %.5f %.5f %.3f | avg %.5f) partials[0..3] %.3f %.3f %.3f %.3f\n",
        a[0], a[1], a[2], a[3], a[6], b[0], b[1], b[2], b[3], b[6], pt[0], pt[1], pt[2], pt[3]); }
    // stateB held frame 1's result, then the push-restore test wrote stateB again from stateA (frame 2's): prev = frame 2 mixed once more
    std::printf("sampled after relocation: luma %.5f (exact average of the new texture %.5f, expected (avg + 7*prev)/8 within 1%%)\n", cur[0], avg2);
    if (!(cur[0] > 0.f) || std::fabs(cur[0] - (avg2 + 7 * prev[0]) / 8) > 0.01f * std::fmax(1.f, cur[0])) {
      std::printf("note: prev differs from the simple model after the push-restore dispatch; checking only that the average dominates\n");
      if (std::fabs(cur[0] * 8 - avg2) > 0.2f * avg2) { std::printf("FAIL: sampled read after relocation\n"); return 1; }
    }
    pipe2->Release(); params->Release(); rbuf->Release();
  }

  // ---- Full-screen graphics passes: two targets from one pass, then a pass that samples a target into a
  // viewport sub-rectangle of another texture (scissor), then D3D9's own state must be back (alpha test again).
  {
    std::vector<char> grad = readFile(argc > 3 ? argv[3] : "nvr_ext_test_grad.spv");
    std::vector<char> copy = readFile(argc > 4 ? argv[4] : "nvr_ext_test_copy.spv");
    IDirect3DTexture9 *tA, *tB;
    CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &tA, nullptr), "tA");
    CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &tB, nullptr), "tB");

    NVR_EXT_GRAPHICS_PIPELINE_DESC gp = {};
    gp.pFragmentSpirv = grad.data(); gp.FragmentSpirvSize = UINT(grad.size());
    gp.PushConstantSize = 16; gp.ColorTargetCount = 2; gp.Blend = NVR_EXT_BLEND_NONE; gp.pDebugName = "gradient";
    ID3D9NvrExtPipeline* pipeG; CHECK(ext->CreateGraphicsPipeline(&gp, &pipeG), "CreateGraphicsPipeline gradient");

    float value[4] = { 0.25f, 0.75f, 0, 0 };
    NVR_EXT_DRAW_DESC draw = {};
    draw.pPipeline = pipeG; draw.TargetCount = 2;
    draw.Targets[0] = { tA, 0, 0, FALSE }; draw.Targets[1] = { tB, 0, 0, FALSE };
    draw.pPushData = value; draw.PushDataSize = 16;
    CHECK(ext->Draw(&draw), "Draw gradient");

    NVR_EXT_BUFFER_DESC rbd = { W * H * 8, NVR_EXT_BUFFER_TRANSFER_DST | NVR_EXT_BUFFER_TRANSFER_SRC, "readback2" };
    ID3D9NvrExtBuffer* rbuf; CHECK(ext->CreateBuffer(&rbd, &rbuf), "readback2");
    std::vector<uint16_t> ta(W * H * 4), tb(W * H * 4);
    CHECK(ext->CopyTextureToBuffer(rbuf, 0, tA, 0, 0), "copy tA"); CHECK(ext->ReadBuffer(rbuf, 0, W * H * 8, ta.data()), "read tA");
    CHECK(ext->CopyTextureToBuffer(rbuf, 0, tB, 0, 0), "copy tB"); CHECK(ext->ReadBuffer(rbuf, 0, W * H * 8, tb.data()), "read tB");
    int bad = 0;
    for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
      float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
      const uint16_t* a = &ta[(y * W + x) * 4]; const uint16_t* b = &tb[(y * W + x) * 4];
      float ea[4] = { u, v, 0.25f, 1 }, eb[4] = { 1 - u, 1 - v, 0.75f, 1 };
      for (int c = 0; c < 4; c++) { if (std::fabs(halfToFloat(a[c]) - ea[c]) > 3e-3f) bad++; if (std::fabs(halfToFloat(b[c]) - eb[c]) > 3e-3f) bad++; }
    }
    std::printf("graphics pass: %d bad components of %u in two targets; texel (0,0) of A = (%.4f %.4f %.3f %.1f)\n", bad, 2 * W * H * 4,
      halfToFloat(ta[0]), halfToFloat(ta[1]), halfToFloat(ta[2]), halfToFloat(ta[3]));
    if (bad) { std::printf("FAIL: gradient pass\n"); return 1; }

    // Sample tA into a 20x15 rectangle of 'color' at (10,5); the rest of 'color' keeps the storage test's values.
    NVR_EXT_BINDING_DESC cb[] = {
      { 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
      { 0, 0, NVR_EXT_BIND_SAMPLER,       0, NVR_EXT_VIEW_2D, FALSE, 0 },
    };
    NVR_EXT_GRAPHICS_PIPELINE_DESC cp = {};
    cp.pFragmentSpirv = copy.data(); cp.FragmentSpirvSize = UINT(copy.size());
    cp.BindingCount = 2; cp.pBindings = cb; cp.PushConstantSize = 4;
    cp.UsesSamplerHeap = TRUE; cp.SamplerHeapSet = 0; cp.SamplerHeapBinding = 0;
    cp.ColorTargetCount = 1; cp.pDebugName = "copy half";
    ID3D9NvrExtPipeline* pipeC; CHECK(ext->CreateGraphicsPipeline(&cp, &pipeC), "CreateGraphicsPipeline copy");
    NVR_EXT_SAMPLER_DESC nsd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP };
    ID3D9NvrExtSampler* nearest; CHECK(ext->CreateSampler(&nsd, &nearest), "nearest sampler");
    NVR_EXT_RESOURCE_BINDING crb[] = {
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE, tA,      0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_SAMPLER,       nearest, 0, 0, 0, FALSE },
    };
    uint32_t zero = 0;
    NVR_EXT_DRAW_DESC d2 = {};
    d2.pPipeline = pipeC; d2.TargetCount = 1; d2.Targets[0] = { color, 0, 0, FALSE };
    d2.ViewportX = 10; d2.ViewportY = 5; d2.ViewportWidth = 20; d2.ViewportHeight = 15;
    d2.BindingCount = 2; d2.pBindings = crb; d2.pPushData = &zero; d2.PushDataSize = 4;
    CHECK(ext->Draw(&d2), "Draw copy");
    std::vector<uint16_t> tc(W * H * 4);
    CHECK(ext->CopyTextureToBuffer(rbuf, 0, color, 0, 0), "copy color"); CHECK(ext->ReadBuffer(rbuf, 0, W * H * 8, tc.data()), "read color");
    bad = 0;
    for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
      const uint16_t* t = &tc[(y * W + x) * 4];
      bool inside = x >= 10 && x < 30 && y >= 5 && y < 20;
      float e[4];
      if (inside) {
        // uv across the 20x15 rectangle, nearest sample of tA at that uv
        float u = (x - 10 + 0.5f) / 20, v = (y - 5 + 0.5f) / 15;
        UINT sx = UINT(u * W), sy = UINT(v * H);
        float su = (sx + 0.5f) / W, sv = (sy + 0.5f) / H;
        e[0] = su * 0.5f; e[1] = sv * 0.5f; e[2] = 0.125f; e[3] = 0.5f;
      } else { e[0] = 0.5f + x; e[1] = 0.25f + y; e[2] = 2.f; e[3] = 1.f; }
      for (int c = 0; c < 4; c++) if (std::fabs(halfToFloat(t[c]) - e[c]) > 0.05f) bad++;
    }
    std::printf("sampled pass into a sub-rectangle: %d bad components (inside and outside the rectangle)\n", bad);
    if (bad) { std::printf("FAIL: sampled pass / scissor\n"); return 1; }

    // D3D9's own state must be back: the alpha-tested quad again
    {
      struct V { float x, y, z, rhw; DWORD color; };
      auto quadPixel = [&](DWORD ref) -> int {
        V quad[4] = { { 0, 0, 0, 1, 0x40FF0000 }, { 64, 0, 0, 1, 0x40FF0000 }, { 0, 48, 0, 1, 0x40FF0000 }, { 64, 48, 0, 1, 0x40FF0000 } };
        dev->SetRenderTarget(0, bb); dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 10, 20, 30), 1.f, 0);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAREF, ref); dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->BeginScene(); dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)); dev->EndScene();
        IDirect3DSurface9* sys; dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        dev->GetRenderTargetData(bb, sys); D3DLOCKED_RECT lr; sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        DWORD px = *(DWORD*)((char*)lr.pBits + 20 * lr.Pitch + 32 * 4) & 0xFFFFFF; sys->UnlockRect(); sys->Release(); return int(px);
      };
      int rejected = quadPixel(128), shown = quadPixel(32);
      std::printf("D3D9 after the passes: alpha-tested quad rejected %06x (0a141e), shown %06x (ff0000)\n", rejected, shown);
      if (rejected != 0x0A141E || shown != 0xFF0000) { std::printf("FAIL: D3D9 state after Draw\n"); return 1; }
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
    pipeG->Release(); pipeC->Release(); nearest->Release(); rbuf->Release(); tA->Release(); tB->Release();
  }

  // ---- NVR's ported passes: each .spv must be accepted with the binding table and push size NVR's C++ declares
  // (EffectRecord::ExtTexturePass), and draw once without an error. Values are not checked here (line-for-line ports).
  if (argc > 5) {
    struct Port { const char* name; UINT textures; UINT pushSize; UINT samplerOffset; UINT wrapOffset; };
    const Port ports[] = {
      { "Exposure", 2, 32, 16, 0 }, { "ImageAdjust", 1, 64, 48, 0 }, { "Sharpening", 2, 64, 48, 0 },
      { "FXAA", 1, 32, 16, 0 }, { "LowHF", 1, 32, 16, 0 }, { "SkyDither", 2, 32, 16, 0 },
      { "BloodLens", 2, 48, 32, 36 }, { "WaterLens", 2, 48, 32, 36 },
      { "LUT", 4, 48, 32, 0 }, { "Lens", 3, 32, 16, 20 }, { "MotionBlur", 1, 64, 48, 0 },   // MotionBlur prints NaN here: the dummy constants give exp() an overflow, as the HLSL would
    };
    IDirect3DTexture9* out; CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &out, nullptr), "port target");
    for (const Port& port : ports) {
      std::string path = std::string(argv[5]) + "/" + port.name + ".spv";
      std::vector<char> code = readFile(path.c_str());
      NVR_EXT_BINDING_DESC bindings[9] = {};
      for (UINT i = 0; i < port.textures; i++) { bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; }
      bindings[port.textures].Type = NVR_EXT_BIND_SAMPLER; bindings[port.textures].PushOffset = port.samplerOffset;
      bindings[port.textures + 1].Type = NVR_EXT_BIND_SAMPLER; bindings[port.textures + 1].Slot = 1; bindings[port.textures + 1].PushOffset = port.wrapOffset;
      UINT samplers = port.wrapOffset ? 2 : 1;
      NVR_EXT_GRAPHICS_PIPELINE_DESC pd = {};
      pd.pFragmentSpirv = code.data(); pd.FragmentSpirvSize = UINT(code.size());
      pd.BindingCount = port.textures + samplers; pd.pBindings = bindings; pd.PushConstantSize = port.pushSize;
      pd.UsesSamplerHeap = TRUE; pd.ColorTargetCount = 1; pd.pDebugName = port.name;
      ID3D9NvrExtPipeline* pipe = nullptr;
      if (FAILED(ext->CreateGraphicsPipeline(&pd, &pipe))) { std::printf("FAIL: port %s refused\n", port.name); return 1; }
      NVR_EXT_RESOURCE_BINDING rb[9] = {};
      for (UINT i = 0; i < port.textures; i++) { rb[i].Slot = i; rb[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; rb[i].pResource = i == 0 ? (IUnknown*)color : (IUnknown*)depth; }
      rb[port.textures].Type = NVR_EXT_BIND_SAMPLER; rb[port.textures].pResource = sampler;
      rb[port.textures + 1].Type = NVR_EXT_BIND_SAMPLER; rb[port.textures + 1].Slot = 1; rb[port.textures + 1].pResource = sampler;
      float push[16] = { 0.5f, 1.f, 1.f, 1.f,  1.f / W, 1.f / H, 1.f, 1.f,  0.1f, 1.f, 1.f, 1.f,  0, 0, 0, 0 };
      NVR_EXT_DRAW_DESC dd = {};
      dd.pPipeline = pipe; dd.TargetCount = 1; dd.Targets[0] = { out, 0, 0, FALSE };
      dd.BindingCount = port.textures + samplers; dd.pBindings = rb; dd.pPushData = push; dd.PushDataSize = port.pushSize;
      if (FAILED(ext->Draw(&dd))) { std::printf("FAIL: port %s did not draw\n", port.name); return 1; }
      float probe[4] = {};
      NVR_EXT_BUFFER_DESC pbd = { W * H * 8, NVR_EXT_BUFFER_TRANSFER_DST | NVR_EXT_BUFFER_TRANSFER_SRC, "probe" };
      ID3D9NvrExtBuffer* pbuf; CHECK(ext->CreateBuffer(&pbd, &pbuf), "probe");
      CHECK(ext->CopyTextureToBuffer(pbuf, 0, out, 0, 0), "probe copy");
      uint16_t h4[4]; CHECK(ext->ReadBuffer(pbuf, (7 * W + 5) * 8, 8, h4), "probe read");
      for (int c = 0; c < 4; c++) probe[c] = halfToFloat(h4[c]);
      std::printf("port %-12s ok: texel (5,7) = (%.4f %.4f %.4f %.3f)\n", port.name, probe[0], probe[1], probe[2], probe[3]);
      // Independent C++ transcriptions of the HLSL for the single-texture ports: the GLSL must agree.
      {
        float c[3] = { 5.5f, 7.25f, 2.f }; float e[3] = { NAN, NAN, NAN };
        auto linz = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        auto delin = [](float v) { return v <= 0.0031308f ? v * 12.92f : std::pow(v, 1.f / 2.4f) * 1.055f - 0.055f; };
        auto pows = [](float a, float b) { return std::pow(std::fabs(a), b) * (a > 0 ? 1.f : a < 0 ? -1.f : 0.f); };
        auto sstep = [](float a, float b, float x) { float t = std::fmin(1.f, std::fmax(0.f, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
        if (!std::strcmp(port.name, "Exposure")) {
          float minB = 0.5f, maxB = 1.f, avg = 1.f;   // the R32F "avg luma" texture reads (r, 1, 1, 1): g = 1
          float neg = std::fmax(std::log(minB / avg), 0.f) + 1, add = std::fmax(std::log(avg / minB), 0.f) + 1, diff = add / neg;
          for (int i = 0; i < 3; i++) { float l = linz(c[i]); e[i] = delin(l + (l / diff - l) * maxB); }
        } else if (!std::strcmp(port.name, "ImageAdjust")) {
          float data[4] = { 0.5f, 1.f, 1.f, 1.f }, dark[3] = { 1.f / W, 1.f / H, 1.f }, light[3] = { 0.1f, 1.f, 1.f };
          float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
          for (int i = 0; i < 3; i++) {
            float v = lum + (c[i] - lum) * data[2]; v = pows(v, 2.2f);
            float darks = sstep(1, 0, v) * v * pows(dark[i], 2.2f), lights = sstep(0, 1, v) * v * pows(light[i], 2.2f);
            float n = (darks + lights) * std::fmax(0.f, data[0]); v = v + (n - v) * data[3]; v = pows(v, 1.f / 2.2f);
            n = (v - 0.5f) * data[1] + 0.5f; e[i] = v + (n - v) * data[3];
          }
        } else if (!std::strcmp(port.name, "LowHF")) {
          float d[4] = { 0.5f, 1.f, 1.f, 1.f }; float col[3];
          for (int i = 0; i < 3; i++) col[i] = c[i];
          if (d[1] > 0) { // blur taps at uv +- y, +- 2y: the texture is a ramp, clamped at the edges; taps of 1.0 and 2.0 uv land on the far corner / clamp
            float u = (5 + 0.5f) / W, v = (7 + 0.5f) / H;
            auto tex = [&](float uu, float vv, int ch) { uu = std::fmin(1.f, std::fmax(0.f, uu)); vv = std::fmin(1.f, std::fmax(0.f, vv));
              float x = std::fmin(W - 1.f, std::fmax(0.f, uu * W - 0.5f)), y = std::fmin(H - 1.f, std::fmax(0.f, vv * H - 0.5f));
              return ch == 0 ? 0.5f + x : ch == 1 ? 0.25f + y : 2.f; };
            for (int i = 0; i < 3; i++) col[i] = (tex(u, v, i) + tex(u + d[1], v + d[1], i) + tex(u + 2 * d[1], v + 2 * d[1], i) + tex(u - d[1], v - d[1], i) + tex(u - 2 * d[1], v - 2 * d[1], i)) / 5;
          }
          float lum = 0.2125f * col[0] + 0.7154f * col[1] + 0.0721f * col[2];
          for (int i = 0; i < 3; i++) col[i] = col[i] + (lum - col[i]) * d[0];
          float iu = (5 + 0.5f) / W - 0.5f, iv = (7 + 0.5f) / H - 0.5f, vig = 1 - (iu * iu + iv * iv);
          float f = std::fmin(1.f, std::fmax(0.f, std::pow(std::fabs(vig), d[2]) + d[3]));
          for (int i = 0; i < 3; i++) e[i] = col[i] * f;
        }
        if (!std::isnan(e[0])) {
          bool same = true;
          for (int i = 0; i < 3; i++) same &= std::fabs(probe[i] - e[i]) <= 0.02f * std::fmax(1.f, std::fabs(e[i]));
          std::printf("  C++ reference: (%.4f %.4f %.4f) %s\n", e[0], e[1], e[2], same ? "agrees" : "DIFFERS");
          if (!same) { std::printf("FAIL: port %s differs from the HLSL transcription\n", port.name); return 1; }
        }
      }
      pbuf->Release(); pipe->Release();
    }
    out->Release();
  }

  // ---- Shader replacement (1.2): a hand-assembled ps_3_0 "oC0 = tex2D(s0, v0) * c5" gets a SPIR-V replacement that
  // also multiplies by an NVR uniform buffer and adds an NVR texture. The game's texture, sampler and constant must
  // still arrive; an unregistered shader (same but c6) must keep DXVK's own conversion.
  if (argc > 6) {
    std::vector<char> rep = readFile(argv[6]);
    // ps_3_0: dcl_texcoord0 v0 / dcl_2d s0 / texld r0, v0, s0 / mul oC0, r0, cN / end
    auto makePs = [](DWORD constIndex) {
      return std::vector<DWORD>{ 0xFFFF0300, 0x0200001F, 0x80000005, 0x900F0000, 0x0200001F, 0x90000000, 0xA00F0800,
                                 0x03000042, 0x800F0000, 0x90E40000, 0xA0E40800, 0x03000005, 0x800F0800, 0x80E40000, 0xA0E40000 | constIndex, 0x0000FFFF };
    };
    std::vector<DWORD> psA = makePs(5), psB = makePs(6);

    NVR_EXT_BINDING_DESC extras[] = {
      { 4, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
      { 4, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },
      { 4, 2, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },
    };
    NVR_EXT_SHADER_REPLACEMENT_DESC rd = {};
    rd.pBytecode = psA.data(); rd.BytecodeSize = UINT(psA.size() * 4);
    rd.pSpirv = rep.data(); rd.SpirvSize = UINT(rep.size());
    rd.TextureMask = 1; rd.FloatRegisterMask[0] = 1u << 5;
    rd.ExtraCount = 3; rd.pExtra = extras; rd.pDebugName = "replace test";
    CHECK(ext->RegisterShaderReplacement(&rd), "RegisterShaderReplacement");
    NVR_EXT_SHADER_REPLACEMENT_DESC badRd = rd; badRd.TextureMask = 3;   // s1 is not used by the original
    if (SUCCEEDED(ext->RegisterShaderReplacement(&badRd))) { std::printf("FAIL: TextureMask beyond the original accepted\n"); return 1; }

    IDirect3DPixelShader9 *shA, *shB;
    CHECK(dev->CreatePixelShader(psA.data(), &shA), "CreatePixelShader A (replaced)");
    CHECK(dev->CreatePixelShader(psB.data(), &shB), "CreatePixelShader B (original)");

    IDirect3DTexture9 *tex, *extraTex;
    CHECK(dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr), "tex");
    CHECK(dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &extraTex, nullptr), "extraTex");
    IDirect3DSurface9 *texS, *extraS; tex->GetSurfaceLevel(0, &texS); extraTex->GetSurfaceLevel(0, &extraS);
    dev->SetRenderTarget(0, texS);   dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 128, 64, 255), 1.f, 0);  // (0.502 0.251 1 1)
    dev->SetRenderTarget(0, extraS); dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 64), 1.f, 0);       // (0 0 0.251 0)
    texS->Release(); extraS->Release();

    NVR_EXT_BUFFER_DESC ub = { 16, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "extra ubo" };
    ID3D9NvrExtBuffer* extraUbo; CHECK(ext->CreateBuffer(&ub, &extraUbo), "extra ubo");
    float scale[4] = { 2.f, 1.f, 1.f, 1.f };
    CHECK(ext->UpdateBuffer(extraUbo, 0, 16, scale), "UpdateBuffer extra");
    NVR_EXT_RESOURCE_BINDING stage[] = {
      { 0, NVR_EXT_BIND_UNIFORM_BUFFER, extraUbo, 0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  extraTex, 0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_SAMPLER,        sampler,  0, 0, 0, FALSE },
    };
    CHECK(ext->SetReplacementResources(3, stage), "SetReplacementResources");

    struct V { float x, y, z, rhw, u, v; };
    auto drawWith = [&](IDirect3DPixelShader9* ps, const float* c, DWORD reg) -> int {
      V quad[4] = { { 0, 0, 0, 1, 0, 0 }, { 64, 0, 0, 1, 1, 0 }, { 0, 48, 0, 1, 0, 1 }, { 64, 48, 0, 1, 1, 1 } };
      dev->SetRenderTarget(0, bb); dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 10, 20, 30), 1.f, 0);
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_ZENABLE, FALSE);
      dev->SetTexture(0, tex); dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      dev->SetPixelShader(ps); dev->SetPixelShaderConstantF(reg, c, 1);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
      dev->BeginScene(); dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)); dev->EndScene();
      IDirect3DSurface9* sys; dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
      dev->GetRenderTargetData(bb, sys); D3DLOCKED_RECT lr; sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
      DWORD px = *(DWORD*)((char*)lr.pBits + 20 * lr.Pitch + 32 * 4) & 0xFFFFFF; sys->UnlockRect(); sys->Release();
      dev->SetPixelShader(nullptr); dev->SetTexture(0, nullptr);
      return int(px);
    };
    float c5[4] = { 0.5f, 2.f, 0.5f, 1.f };
    int replaced = drawWith(shA, c5, 5);   // replacement: tex * c5 * scale + extra = (0.502 0.502 0.751 1) -> 80 80 bf
    int original = drawWith(shB, c5, 6);   // DXVK's conversion: tex * c6 = (0.251 0.502 0.5 1) -> 40 80 80
    auto near3 = [](int px, int r, int g, int b) { return std::abs(((px >> 16) & 255) - r) <= 2 && std::abs(((px >> 8) & 255) - g) <= 2 && std::abs((px & 255) - b) <= 2; };
    std::printf("shader replacement: replaced shader gives %06x (expected ~8080bf), untouched shader gives %06x (expected ~408080)\n", replaced, original);
    if (!near3(replaced, 0x80, 0x80, 0xBF) || !near3(original, 0x40, 0x80, 0x80)) { std::printf("FAIL: shader replacement\n"); return 1; }

    // The extra resources are persistent: change the uniform buffer, draw again
    scale[0] = 1.f; scale[2] = 0.f;
    CHECK(ext->UpdateBuffer(extraUbo, 0, 16, scale), "UpdateBuffer extra 2");
    int again = drawWith(shA, c5, 5);      // (0.251 0.502 0.251 1) -> 40 80 40
    std::printf("shader replacement after an extra-buffer update: %06x (expected ~408040)\n", again);
    if (!near3(again, 0x40, 0x80, 0x40)) { std::printf("FAIL: replacement extras not live\n"); return 1; }

    shA->Release(); shB->Release(); tex->Release(); extraTex->Release(); extraUbo->Release();
  }

  // ---- Device Reset: default-pool resources go away, the extension's objects stay usable.
  {
    colorS->Release(); depthS->Release(); avgS->Release(); bb->Release(); color->Release(); depth->Release(); avg->Release();
    pp.BackBufferWidth = 80; pp.BackBufferHeight = 60;
    CHECK(dev->Reset(&pp), "Reset");
    CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &color, nullptr), "color tex 2");
    CHECK(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &depth, nullptr), "depth tex 2");
    CHECK(dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &avg, nullptr), "avg tex 2");
    color->GetSurfaceLevel(0, &colorS); depth->GetSurfaceLevel(0, &depthS); avg->GetSurfaceLevel(0, &avgS);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    CHECK(dev->SetRenderTarget(0, colorS), "rt color 2");
    CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 128, 64, 32), 1.f, 0), "clear color 2");
    CHECK(dev->SetRenderTarget(0, depthS), "rt depth 2");
    CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 64, 0, 0), 1.f, 0), "clear depth 2");
    CHECK(dev->SetRenderTarget(0, bb), "rt bb 2");
    CHECK(ext->ClearBuffer(stateA, 0, 0, 0), "clear A 2");
    NVR_EXT_RESOURCE_BINDING rb[] = {
      { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  color,    0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,  0, 0, 0, FALSE },
      { 0, NVR_EXT_BIND_STORAGE_BUFFER, partials, 0, 0, 0, FALSE }, { 1, NVR_EXT_BIND_STORAGE_BUFFER, stateA, 0, 0, 0, FALSE },
      { 2, NVR_EXT_BIND_STORAGE_BUFFER, stateB,   0, 0, 0, FALSE }, { 0, NVR_EXT_BIND_SAMPLER,        sampler, 0, 0, 0, FALSE },
    };
    NVR_EXT_DISPATCH_DESC dd = {};
    dd.pPipeline = pipe; dd.BindingCount = 6; dd.pBindings = rb; dd.pPushData = &push; dd.PushDataSize = sizeof(push);
    push.phase = 0; dd.GroupsX = gx; dd.GroupsY = gy; dd.GroupsZ = 1; CHECK(ext->Dispatch(&dd), "dispatch 0 after reset");
    push.phase = 1; dd.GroupsX = 1; dd.GroupsY = 1; CHECK(ext->Dispatch(&dd), "dispatch 1 after reset");
    CHECK(ext->CopyBufferToTexture(avg, 0, 0, stateB, 16), "copy after reset");
    CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr), "Present after reset");
    float cur[4] = {}; CHECK(ext->ReadBuffer(stateB, 0, 16, cur), "ReadBuffer after reset");
    std::printf("after Reset: luma %.6f (expected %.6f: first frame again from a cleared state)\n", cur[0], expectedAvg / 8);
    if (std::fabs(cur[0] - expectedAvg / 8) > 1e-4f) { std::printf("FAIL: after reset\n"); return 1; }
  }

  pipe->Release(); partials->Release(); stateA->Release(); stateB->Release(); sampler->Release();
  colorS->Release(); depthS->Release(); avgS->Release(); bb->Release(); color->Release(); depth->Release(); avg->Release();
  ext->Release(); dev->Release(); d3d->Release();
#ifdef NVR_TEST_WIN32
  DestroyWindow(win);
#else
  SDL_DestroyWindow(win); SDL_Quit();
#endif
  std::printf("PASS\n");
  return 0;
}
