// End-to-end test of NVR's ray tracing path on a real GPU, without the game: this 32-bit program plays NVR (a D3D9 device
// through the NVR DXVK fork's d3d9.dll next to it), NvrRtHelper.exe (64-bit) is the helper. Covers the fork's geometry
// staging (ID3D9NvrExtRt::StageGeometry / FlushStagedGeometry: three vertex formats, 16/32-bit lists and strips, base
// vertex, no index buffer), patch 0026's sharing (images, timeline semaphores, CopyFromShared), the protocol, the helper's
// builds / scene / pass, then a moved instance, a released geometry, 300 frames of churn and a geometry rebuilt in place.
// Every ray is checked against the expected hit (instance id and distance).
//
// Build (WSL, MinGW-w64 i686):
//   i686-w64-mingw32-g++ -O2 -std=c++17 -I<fork>/src/d3d9 -I<repo>/tools/nvr-rt-helper -o rt_helper_test.exe rt_helper_test.cpp -ld3d9 -static
// Run (the fork's d3d9.dll next to it):  rt_helper_test.exe <NvrRtHelper.exe> <rt_helper_test.spv> ["<Vulkan device name>"]
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9_nvr_ext_api.h"
#include "NvrRtProtocol.h"

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static uint16_t toHalf(float f) {
  uint32_t x; std::memcpy(&x, &f, 4);
  uint32_t s = (x >> 16) & 0x8000, e = ((x >> 23) & 0xff), m = x & 0x7fffff;
  if (e == 0) return uint16_t(s);
  int ne = int(e) - 127 + 15;
  if (ne <= 0) return uint16_t(s);
  if (ne >= 31) return uint16_t(s | 0x7c00);
  return uint16_t(s | (ne << 10) | (m >> 13));
}

#define CHECK(hr, what) do { HRESULT _hr = (hr); if (FAILED(_hr)) { std::printf("FAIL: %s -> 0x%08x\n", what, unsigned(_hr)); return 1; } } while (0)

static const uint32_t N = 16;

struct PassConstants { uint32_t size[4]; float area[4], origin[4], direction[4]; };
struct Rect { float x0, y0, x1, y1, z; int id; int geometry = 0; float objectZ = 0.f; };

static const Rect* expect(const std::vector<Rect>& rects, float x, float y, float oz, float dz, int& id, float& t) {
  id = -1; t = -1.f; float best = 1e30f; const Rect* hit = nullptr;
  for (const Rect& r : rects) {
    if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
    float d = (r.z - oz) * dz;
    if (d >= 0.f && d < best) { best = d; t = d; id = r.id; hit = &r; }
  }
  return hit;
}

struct Ctx {
  IDirect3DDevice9* dev;
  ID3D9NvrExtShare* share;
  ID3D9NvrExtRt* rt;
  ID3D9NvrExtSharedImage* arena;
  ID3D9NvrExtSharedImage* output;
  ID3D9NvrExtSharedImage* fetched;
  ID3D9NvrExtSharedImage* probe;
  IDirect3DSurface9* probeRt;
  IDirect3DSurface9* probeSys;
  IDirect3DSurface9* fetchRt;
  IDirect3DSurface9* fetchSys;
  ID3D9NvrExtSharedFence* inputsReady;
  ID3D9NvrExtSharedFence* outputReady;
  IDirect3DTexture9* readTex;
  IDirect3DSurface9* readRt;
  IDirect3DSurface9* readSys;
  NvrRtShared* shared;
  HANDLE frameEvent;
  uint64_t frame;
};

// One frame: the record (ops, instances, one pass), the signal, the wait, the output back; the rays checked.
static int runFrame(Ctx& c, const std::vector<NvrRtGeometryOp>& ops, uint32_t arenaTexels, const std::vector<NvrRtInstance>& inst,
                    const std::vector<Rect>& rects, float oz, float dz, const char* label, bool check) {
  const uint64_t n = ++c.frame;
  // the slot NVR reuses must have been read by the helper (NvrRtProtocol.h)
  for (int i = 0; i < 5000 && c.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
  NvrRtFrame& f = c.shared->frames[n % NVR_RT_FRAME_RING];
  f.frame = n;
  LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); f.writtenQpc = qpc.QuadPart;
  f.opCount = uint32_t(ops.size());
  for (size_t i = 0; i < ops.size(); i++) f.ops[i] = ops[i];
  f.arenaTexels = arenaTexels;
  f.instanceCount = uint32_t(inst.size());
  for (size_t i = 0; i < inst.size(); i++) f.instances[i] = inst[i];
  f.passCount = 1;
  NvrRtPass& p = f.passes[0];
  std::memset(&p, 0, sizeof(p));
  p.shader = 0; p.groupsX = N / 8; p.groupsY = N / 8; p.groupsZ = 1;
  p.imageCount = 4; p.images[0] = 1; p.images[1] = 2; p.images[2] = 3; p.images[3] = 6; p.writeMask = 0xB;
  PassConstants pc = {};
  pc.size[0] = N;
  pc.area[0] = -8.f; pc.area[1] = -8.f; pc.area[2] = 8.f; pc.area[3] = 8.f;
  pc.origin[2] = oz; pc.origin[3] = 100.f; pc.direction[2] = dz;
  p.constantsSize = sizeof(pc);
  std::memcpy(p.constants, &pc, sizeof(pc));
  MemoryBarrier();
  InterlockedExchange64((volatile LONG64*)&c.shared->latestFrame, (LONG64)n);
  SetEvent(c.frameEvent);

  CHECK(c.share->SignalShared(c.inputsReady, n), "SignalShared");
  CHECK(c.share->WaitShared(c.outputReady, n), "WaitShared");
  if (!check) {
    c.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0);
    c.dev->Present(nullptr, nullptr, nullptr, nullptr);
    return 0;
  }
  CHECK(c.share->CopyFromShared(c.readRt, 0, 0, c.output, nullptr), "CopyFromShared");
  CHECK(c.share->CopyFromShared(c.fetchRt, 0, 0, c.fetched, nullptr), "CopyFromShared (fetched)");
  CHECK(c.dev->GetRenderTargetData(c.readRt, c.readSys), "GetRenderTargetData");
  CHECK(c.dev->GetRenderTargetData(c.fetchRt, c.fetchSys), "GetRenderTargetData (fetched)");
  D3DLOCKED_RECT lr, lf;
  CHECK(c.readSys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "LockRect");
  CHECK(c.fetchSys->LockRect(&lf, nullptr, D3DLOCK_READONLY), "LockRect (fetched)");
  int bad = 0, hits = 0;
  for (uint32_t j = 0; j < N; j++) for (uint32_t i = 0; i < N; i++) {
    const float* g = (const float*)((const char*)lr.pBits + j * lr.Pitch) + i * 2;
    float x = -8.f + 16.f * (i + 0.5f) / N, y = -8.f + 16.f * (j + 0.5f) / N;
    int eid; float et; const Rect* hit = expect(rects, x, y, oz, dz, eid, et);
    const int gid = g[0] < -0.5f ? -1 : int(g[0] + 0.5f);
    const float* h = (const float*)((const char*)lf.pBits + j * lf.Pitch) + i * 2;
    hits += gid >= 0;
    bool ok = gid == eid && (eid < 0 || std::fabs(g[1] - et) < 1e-3f);
    // v2: the record (geometry id) and the hit triangle's vertex fetched from the helper's geometry data (object-space z)
    if (ok && hit && hit->geometry) ok = int(h[0] + 0.5f) == hit->geometry && std::fabs(h[1] - hit->objectZ) < 1e-3f;
    if (!ok && bad++ < 6) std::printf("  %s mismatch at (%.1f, %.1f): got id %d t %.4f (record geometry %.0f, vertex z %.3f), expected id %d t %.4f\n",
      label, x, y, gid, g[1], h[0], h[1], eid, et);
  }
  c.readSys->UnlockRect();
  c.fetchSys->UnlockRect();
  // The screen-size image's texels as the helper read them (the layouts of the two drivers agree)
  CHECK(c.share->CopyFromShared(c.probeRt, 0, 0, c.probe, nullptr), "CopyFromShared (probe)");
  CHECK(c.dev->GetRenderTargetData(c.probeRt, c.probeSys), "GetRenderTargetData (probe)");
  D3DLOCKED_RECT lp;
  CHECK(c.probeSys->LockRect(&lp, nullptr, D3DLOCK_READONLY), "LockRect (probe)");
  int badProbe = 0;
  for (uint32_t j = 0; j < N; j++) for (uint32_t i = 0; i < N; i++) {
    const float got = ((const float*)((const char*)lp.pBits + j * lp.Pitch))[i * 2];
    const float want = float(i * 79) + 4096.f * float(j * 45);
    if (got != want && badProbe++ < 3) std::printf("  %s: screen image texel (%u, %u) read %.0f, written %.0f\n", label, i * 79, j * 45, got, want);
  }
  c.probeSys->UnlockRect();
  if (badProbe) { std::printf("%s: screen-size image read back wrong at %d of %u texels -> FAIL\n", label, badProbe, N * N); bad++; }
  std::printf("%s (frame %llu): %u rays, %d hits, %d mismatches -> %s\n", label, (unsigned long long)n, N * N, hits, bad, bad ? "FAIL" : "PASS");
  c.dev->Present(nullptr, nullptr, nullptr, nullptr);
  return bad ? 1 : 0;
}

static NvrRtInstance instance(uint32_t geometry, float tx, uint32_t id) {
  NvrRtInstance i = {};
  i.transform[0][0] = i.transform[1][1] = i.transform[2][2] = 1.f;
  i.transform[0][3] = tx;
  i.geometryId = geometry; i.customIndex = id; i.mask = 0xFF;
  return i;
}

static bool giveHandle(HANDLE mine, HANDLE child, uint64_t& out) {
  HANDLE remote = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), mine, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) return false;
  out = (uint64_t)(uintptr_t)remote;
  return true;
}

static void describe(ID3D9NvrExtSharedImage* img, NvrRtImageDesc& d) {
  NVR_EXT_SHARED_IMAGE_INFO i = {}; img->GetInfo(&i);
  d.allocationSize = i.AllocationSize; d.memoryTypeIndex = i.MemoryTypeIndex; d.vkFormat = i.VkFormat; d.width = i.Width; d.height = i.Height;
  d.usage = i.Usage; d.flags = i.Flags; d.tiling = i.Tiling; d.layout = i.Layout; d.handleType = i.HandleType; d.dedicated = i.Dedicated;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: rt_helper_test.exe <NvrRtHelper.exe> <rt_helper_test.spv> [device]\n"); return 2; }
  if (argc > 3) SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", argv[3]);
  std::printf("NvrRtShared is %u bytes here (32-bit)\n", (unsigned)sizeof(NvrRtShared));

  WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvrrthelpertest";
  RegisterClassA(&wc);
  HWND win = CreateWindowExA(0, "nvrrthelpertest", "nvrrthelpertest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 48, nullptr, nullptr, wc.hInstance, nullptr);
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = win;
  pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  Ctx c = {};
  CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &c.dev), "CreateDevice");
  ID3D9NvrExt* ext = nullptr;
  CHECK(c.dev->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext), "ID3D9NvrExt");
  CHECK(ext->QueryInterface(__uuidof(ID3D9NvrExtShare), (void**)&c.share), "ID3D9NvrExtShare");
  CHECK(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&c.rt), "ID3D9NvrExtRt");
  NVR_EXT_SHARE_INFO si = {}; c.share->GetShareInfo(&si);
  std::printf("device %s (vendor 0x%04X), sharing %s\n", si.DeviceName, si.VendorId, si.Supported ? "yes" : "NO");
  if (!si.Supported) return 1;

  NVR_EXT_SHARED_IMAGE_DESC ad = { 4096, 64, NVR_EXT_SHARED_R32F, "rt arena" };
  NVR_EXT_SHARED_IMAGE_DESC od = { N, N, NVR_EXT_SHARED_RG32F, "rt test output" };
  CHECK(c.share->CreateSharedImage(&ad, &c.arena), "arena");
  CHECK(c.share->CreateSharedImage(&od, &c.output), "output");
  NVR_EXT_SHARED_IMAGE_DESC fd = { N, N, NVR_EXT_SHARED_RG32F, "rt test fetched" };
  CHECK(c.share->CreateSharedImage(&fd, &c.fetched), "fetched");
  { IDirect3DTexture9* t; CHECK(c.dev->CreateTexture(N, N, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &t, nullptr), "fetch tex");
    t->GetSurfaceLevel(0, &c.fetchRt); }
  CHECK(c.dev->CreateOffscreenPlainSurface(N, N, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &c.fetchSys, nullptr), "fetch sys");
  CHECK(c.share->CreateSharedFence(0, &c.inputsReady), "inputsReady");
  CHECK(c.share->CreateSharedFence(0, &c.outputReady), "outputReady");
  CHECK(c.dev->CreateTexture(N, N, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &c.readTex, nullptr), "read tex");
  c.readTex->GetSurfaceLevel(0, &c.readRt);
  CHECK(c.dev->CreateOffscreenPlainSurface(N, N, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &c.readSys, nullptr), "read sys");

  // Shared block + events, then the helper
  wchar_t base[128];
  swprintf(base, 128, L"Local\\NvrRtTest-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrRtShared), base);
  c.shared = mapping ? (NvrRtShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrRtShared)) : nullptr;
  if (!c.shared) { std::printf("FAIL: shared block\n"); return 1; }
  std::memset(c.shared, 0, sizeof(NvrRtShared));
  c.shared->magic = NVR_RT_MAGIC; c.shared->version = NVR_RT_PROTOCOL_VERSION; c.shared->size = sizeof(NvrRtShared);
  c.shared->parentPid = GetCurrentProcessId();
  c.shared->vendorId = si.VendorId;
  std::memcpy(c.shared->deviceLuid, si.DeviceLUID, 8); c.shared->deviceLuidValid = si.DeviceLUIDValid;
  std::memcpy(c.shared->deviceUuid, si.DeviceUUID, 16); std::memcpy(c.shared->driverUuid, si.DriverUUID, 16);
  c.frameEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_RT_EVENT_FRAME_SUFFIX).c_str());
  HANDLE statusEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_RT_EVENT_STATUS_SUFFIX).c_str());
  std::string shader = argv[2];
  { char full[MAX_PATH]; GetFullPathNameA(argv[2], MAX_PATH, full, nullptr); shader = full; }
  std::strncpy(c.shared->shaderPaths[0], shader.c_str(), 259);
  c.shared->shaderCount = 1;

  wchar_t exe[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, argv[1], -1, exe, MAX_PATH);
  std::wstring cmdLine = L"\"" + std::wstring(exe) + L"\" --shm " + base;
  STARTUPINFOW su = { sizeof(su) }; PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe, &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &su, &pi)) {
    std::printf("FAIL: could not start the helper (%lu)\n", GetLastError()); return 1;
  }
  // Screen-size images as the RT sun shadows make them (1280x720 R32F / RGBA16F / R16F): the 2026-10-28 in-game run found the helper's
  // driver (64-bit) wanting another size for them than the game's (32-bit); they must import (unused by the pass)
  ID3D9NvrExtSharedImage* screen[3] = {};
  const NVR_EXT_SHARED_FORMAT screenFormats[3] = { NVR_EXT_SHARED_R32F, NVR_EXT_SHARED_RGBA16F, NVR_EXT_SHARED_R16F };
  for (int i = 0; i < 3; i++) {
    UINT sw = 1280, sh = 720;
    if (const char* size = getenv("RT_TEST_SCREEN")) sscanf(size, "%ux%u", &sw, &sh);	// the import-size experiments (FORK-RT-CORE.md)
    NVR_EXT_SHARED_IMAGE_DESC sd = { sw, sh, screenFormats[i], "rt test screen" };
    CHECK(c.share->CreateSharedImage(&sd, &screen[i]), "screen-size image");
  }
  NVR_EXT_SHARED_IMAGE_DESC pd2 = { N, N, NVR_EXT_SHARED_RG32F, "rt test probe" };
  CHECK(c.share->CreateSharedImage(&pd2, &c.probe), "probe");
  { IDirect3DTexture9* t; CHECK(c.dev->CreateTexture(N, N, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &t, nullptr), "probe tex");
    t->GetSurfaceLevel(0, &c.probeRt); }
  CHECK(c.dev->CreateOffscreenPlainSurface(N, N, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &c.probeSys, nullptr), "probe sys");
  // The pattern x + 4096 y, at the game's screen size (RT_TEST_GAME, else 1280x720), copied into the top left of the shared screen image
  {
    UINT gw = 1280, gh = 720;
    if (const char* size = getenv("RT_TEST_GAME")) sscanf(size, "%ux%u", &gw, &gh);
    IDirect3DTexture9 *sys, *gpu;
    CHECK(c.dev->CreateTexture(gw, gh, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr), "pattern sys");
    CHECK(c.dev->CreateTexture(gw, gh, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &gpu, nullptr), "pattern gpu");
    D3DLOCKED_RECT lr;
    CHECK(sys->LockRect(0, &lr, nullptr, 0), "pattern lock");
    for (UINT y = 0; y < gh; y++) for (UINT x = 0; x < gw; x++) ((float*)((char*)lr.pBits + y * lr.Pitch))[x] = float(x) + 4096.f * float(y);
    sys->UnlockRect(0);
    CHECK(c.dev->UpdateTexture(sys, gpu), "pattern upload");
    CHECK(c.share->CopyToShared(screen[0], 0, 0, gpu, nullptr), "pattern CopyToShared");
  }
  ID3D9NvrExtSharedImage* images[7] = { c.arena, c.output, c.fetched, screen[0], screen[1], screen[2], c.probe };
  c.shared->imageCount = 7;
  for (int i = 0; i < 7; i++) {
    describe(images[i], c.shared->images[i]);
    HANDLE h = nullptr;
    CHECK(images[i]->CreateHandle(&h), "image CreateHandle");
    if (!giveHandle(h, pi.hProcess, c.shared->images[i].handle)) { std::printf("FAIL: DuplicateHandle\n"); return 1; }
  }
  HANDLE h1 = nullptr, h2 = nullptr;
  CHECK(c.inputsReady->CreateHandle(&h1), "fence handle"); CHECK(c.outputReady->CreateHandle(&h2), "fence handle");
  if (!giveHandle(h1, pi.hProcess, c.shared->semInputsReady) || !giveHandle(h2, pi.hProcess, c.shared->semOutputReady)) return 1;
  MemoryBarrier();
  c.shared->setupDone = 1;
  SetEvent(c.frameEvent);
  const ULONGLONG start = GetTickCount64();
  while (c.shared->state == NVR_RT_STATE_STARTING && GetTickCount64() - start < 15000) {
    HANDLE waits[2] = { statusEvent, pi.hProcess };
    if (WaitForMultipleObjects(2, waits, FALSE, 200) == WAIT_OBJECT_0 + 1) break;
  }
  std::printf("helper state %u after %llu ms: %s\n", c.shared->state, (unsigned long long)(GetTickCount64() - start), c.shared->message);
  if (c.shared->state != NVR_RT_STATE_READY) { TerminateProcess(pi.hProcess, 1); return 1; }

  // Geometry (as nvr_rt_test.cpp): ground (float3 + uv, 16-bit list), roof (float4, base vertex 2, 32-bit strip from index 1),
  // post (half4, no index buffer)
  IDirect3DVertexBuffer9 *vb1, *vb2, *vb3; IDirect3DIndexBuffer9 *ib1, *ib2;
  CHECK(c.dev->CreateVertexBuffer(4 * 20, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb1, nullptr), "vb1");
  { float* v; vb1->Lock(0, 0, (void**)&v, 0);
    const float q[4][5] = { { -10, -10, 0, 0, 0 }, { 10, -10, 0, 1, 0 }, { 10, 10, 0, 1, 1 }, { -10, 10, 0, 0, 1 } };
    std::memcpy(v, q, sizeof(q)); vb1->Unlock(); }
  CHECK(c.dev->CreateIndexBuffer(6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib1, nullptr), "ib1");
  { uint16_t* i; ib1->Lock(0, 0, (void**)&i, 0); const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 }; std::memcpy(i, q, sizeof(q)); ib1->Unlock(); }
  CHECK(c.dev->CreateVertexBuffer(6 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb2, nullptr), "vb2");
  { float* v; vb2->Lock(0, 0, (void**)&v, 0);
    const float q[6][4] = { { 99, 99, 99, 1 }, { 99, 99, 99, 1 }, { -2, -2, 5, 1 }, { 2, -2, 5, 1 }, { 2, 2, 5, 1 }, { -2, 2, 5, 1 } };
    std::memcpy(v, q, sizeof(q)); vb2->Unlock(); }
  CHECK(c.dev->CreateIndexBuffer(5 * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib2, nullptr), "ib2");
  { uint32_t* i; ib2->Lock(0, 0, (void**)&i, 0); const uint32_t q[5] = { 777, 0, 1, 3, 2 }; std::memcpy(i, q, sizeof(q)); ib2->Unlock(); }
  CHECK(c.dev->CreateVertexBuffer(6 * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb3, nullptr), "vb3");
  { uint16_t* v; vb3->Lock(0, 0, (void**)&v, 0);
    const float q[6][3] = { { 6, 6, 2 }, { 7, 6, 2 }, { 7, 7, 2 }, { 6, 6, 2 }, { 7, 7, 2 }, { 6, 7, 2 } };
    for (int k = 0; k < 6; k++) { for (int cc = 0; cc < 3; cc++) v[k * 4 + cc] = toHalf(q[k][cc]); v[k * 4 + 3] = toHalf(1.f); }
    vb3->Unlock(); }

  NVR_EXT_RT_RANGE r1 = { NVR_EXT_RT_TRIANGLE_LIST, 0, 2, 0 };
  NVR_EXT_RT_GEOMETRY_DESC g1 = { vb1, 0, 20, 4, NVR_EXT_RT_POSITION_FLOAT3, ib1, 1, &r1, 0, "ground" };
  NVR_EXT_RT_RANGE r2 = { NVR_EXT_RT_TRIANGLE_STRIP, 1, 2, 2 };
  NVR_EXT_RT_GEOMETRY_DESC g2 = { vb2, 0, 16, 6, NVR_EXT_RT_POSITION_FLOAT4, ib2, 1, &r2, 0, "roof" };
  NVR_EXT_RT_RANGE r3 = { NVR_EXT_RT_TRIANGLE_LIST, 0, 2, 0 };
  NVR_EXT_RT_GEOMETRY_DESC g3 = { vb3, 0, 8, 0, NVR_EXT_RT_POSITION_HALF4, nullptr, 1, &r3, 0, "post" };

  // Staged at awkward places: the second starts mid-row, the third on the next row
  std::vector<NvrRtGeometryOp> ops;
  uint32_t texel = 5;
  const NVR_EXT_RT_GEOMETRY_DESC* descs[3] = { &g1, &g2, &g3 };
  for (uint32_t k = 0; k < 3; k++) {
    NVR_EXT_RT_GEOMETRY_INFO gi = {};
    CHECK(c.rt->StageGeometry(descs[k], c.arena, texel, &gi), "StageGeometry");
    NvrRtGeometryOp op = {};
    op.op = NVR_RT_OP_BUILD; op.id = 1 + k; op.vertexCount = gi.VertexCount; op.triangleCount = gi.TriangleCount; op.arenaTexel = texel;
    ops.push_back(op);
    std::printf("staged geometry %u: %u vertices, %u triangles, %u texels at %u\n", k + 1, gi.VertexCount, gi.TriangleCount, gi.Texels, texel);
    texel += gi.Texels + (k == 1 ? 4096 : 3);
  }
  CHECK(c.rt->FlushStagedGeometry(c.arena), "FlushStagedGeometry");

  int failures = 0;
  std::vector<NvrRtInstance> inst = { instance(1, 0, 7), instance(2, 1.f, 9), instance(999, 0, 99), instance(3, 0, 11) };
  std::vector<Rect> rects = { { -10, -10, 10, 10, 0, 7, 1, 0.f }, { -1, -2, 3, 2, 5, 9, 2, 5.f }, { 6, 6, 7, 7, 2, 11, 3, 2.f } };
  failures += runFrame(c, ops, texel, inst, rects, 0.01f, 1.f, "scene 1, rays up", true);
  failures += runFrame(c, {}, 0, inst, rects, 20.f, -1.f, "scene 1, rays down", true);

  // The roof moved, the ground released
  NvrRtGeometryOp release = {}; release.op = NVR_RT_OP_RELEASE; release.id = 1;
  inst = { instance(2, -1.f, 9), instance(3, 0, 11), instance(1, 0, 7) };	// a stale reference to the released ground: skipped
  rects = { { -3, -2, 1, 2, 5, 9, 2, 5.f }, { 6, 6, 7, 7, 2, 11, 3, 2.f } };
  failures += runFrame(c, { release }, 0, inst, rects, 20.f, -1.f, "scene 2 (roof moved, ground released), rays down", true);

  // 300 frames of instance churn, then a check
  for (int k = 0; k < 300; k++) {
    inst[0] = instance(2, float(k % 3) - 1.f, 9);
    if (runFrame(c, {}, 0, inst, rects, 20.f, -1.f, "churn", false)) { failures++; break; }
  }
  inst[0] = instance(2, 1.f, 9);
  rects[0] = { -1, -2, 3, 2, 5, 9, 2, 5.f };
  failures += runFrame(c, {}, 0, inst, rects, 20.f, -1.f, "after 300 frames", true);

  // The post rebuilt in place under its id from a moved copy (half4 again), staged at texel 0
  { uint16_t* v; vb3->Lock(0, 0, (void**)&v, 0); for (int k = 0; k < 6; k++) v[k * 4 + 2] = toHalf(3.f); vb3->Unlock(); }
  NVR_EXT_RT_GEOMETRY_INFO gi = {};
  CHECK(c.rt->StageGeometry(&g3, c.arena, 0, &gi), "StageGeometry (rebuild)");
  CHECK(c.rt->FlushStagedGeometry(c.arena), "FlushStagedGeometry (rebuild)");
  NvrRtGeometryOp rebuild = {}; rebuild.op = NVR_RT_OP_BUILD; rebuild.id = 3; rebuild.vertexCount = gi.VertexCount; rebuild.triangleCount = gi.TriangleCount;
  rects[1] = { 6, 6, 7, 7, 3, 11, 3, 3.f };
  failures += runFrame(c, { rebuild }, gi.Texels, inst, rects, 20.f, -1.f, "post rebuilt at z 3", true);

  std::printf("helper: %u geometries, %llu triangles, %llu KB structures, last %u instances, %u failed ops, shader failures 0x%x; "
    "GPU %.3f ms avg (builds %.3f, scene %.3f, passes %.3f), response %.3f ms avg (max %.3f)\n",
    c.shared->geometries, (unsigned long long)c.shared->triangles, (unsigned long long)(c.shared->geometryBytes >> 10), c.shared->lastInstances,
    c.shared->failedOps, c.shared->shaderFailures, c.shared->gpuTimeUsAvg / 1000.0, c.shared->buildUsAvg / 1000.0, c.shared->sceneUsAvg / 1000.0,
    c.shared->traceUsAvg / 1000.0, c.shared->responseUsAvg / 1000.0, c.shared->responseUsMax / 1000.0);
  if (c.shared->failedOps || c.shared->shaderFailures) failures++;

  c.shared->quit = 1;
  SetEvent(c.frameEvent);
  WaitForSingleObject(pi.hProcess, 3000);
  std::printf("%s (%d failures)\n", failures ? "RT HELPER TEST FAILED" : "RT HELPER TEST PASSED", failures);
  return failures ? 1 : 0;
}
