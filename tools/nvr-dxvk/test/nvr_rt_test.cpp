// End-to-end test of the NVR DXVK ray tracing core (ID3D9NvrExtRt, fork patch 0028) on a real GPU (Windows, MinGW build).
// Three geometries in three vertex formats and three index layouts (16-bit list, 32-bit strip with a base vertex and an
// offset start, no index buffer), a scene of them, rays up and down over a grid from a compute pass that reads the scene
// by its address; then the scene rebuilt with a moved instance, then without a released geometry. Every ray is checked
// against the expected hit (instance id and distance).
//
// Build (WSL, MinGW-w64 i686):  i686-w64-mingw32-g++ -O2 -std=c++17 -I<fork>/src/d3d9 -o nvr_rt_test.exe nvr_rt_test.cpp -ld3d9 -static
// Run next to the fork's d3d9.dll:  nvr_rt_test.exe nvr_rt_test.spv ["<Vulkan device name>"]
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "d3d9_nvr_ext_api.h"

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static std::vector<char> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
  size_t n = size_t(f.tellg()); f.seekg(0);
  std::vector<char> v(n); f.read(v.data(), n); return v;
}

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

struct Push {
  uint32_t sceneLo, sceneHi, size, pad;
  float area[4], origin[4], direction[4];
};

static const uint32_t N = 16;

struct Rect { float x0, y0, x1, y1, z; uint32_t id; bool on; };

// Expected (id, t) of a ray from (x, y, oz) along dz (+1 up, -1 down); id -1 = miss.
static void expect(const std::vector<Rect>& rects, float x, float y, float oz, float dz, int& id, float& t) {
  id = -1; t = 1e30f;
  for (const Rect& r : rects) {
    if (!r.on || x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
    float d = (r.z - oz) * dz;
    if (d >= 0.f && d < t) { t = d; id = int(r.id); }
  }
}

static int checkPass(ID3D9NvrExt* ext, IDirect3DDevice9* dev, ID3D9NvrExtPipeline* pipe, ID3D9NvrExtBuffer* out,
                     ID3D9NvrExtRtScene* scene, const std::vector<Rect>& rects, float oz, float dz, const char* label) {
  Push push = {};
  uint64_t a = scene->GetAddress();
  push.sceneLo = uint32_t(a); push.sceneHi = uint32_t(a >> 32); push.size = N;
  push.area[0] = -8.f; push.area[1] = -8.f; push.area[2] = 8.f; push.area[3] = 8.f;
  push.origin[2] = oz; push.origin[3] = 100.f;
  push.direction[2] = dz;
  NVR_EXT_RESOURCE_BINDING b = {}; b.Slot = 0; b.Type = NVR_EXT_BIND_STORAGE_BUFFER; b.pResource = out;
  NVR_EXT_DISPATCH_DESC d = {}; d.pPipeline = pipe; d.BindingCount = 1; d.pBindings = &b; d.pPushData = &push; d.PushDataSize = sizeof(push);
  d.GroupsX = N / 8; d.GroupsY = N / 8; d.GroupsZ = 1;
  CHECK(ext->Dispatch(&d), "Dispatch");
  dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0); dev->Present(nullptr, nullptr, nullptr, nullptr);
  std::vector<float> r(N * N * 4);
  CHECK(ext->ReadBuffer(out, 0, N * N * 16, r.data()), "ReadBuffer");
  int bad = 0, hits = 0;
  for (uint32_t j = 0; j < N; j++) for (uint32_t i = 0; i < N; i++) {
    float x = -8.f + 16.f * (i + 0.5f) / N, y = -8.f + 16.f * (j + 0.5f) / N;
    int eid; float et; expect(rects, x, y, oz, dz, eid, et);
    const float* g = &r[(j * N + i) * 4];
    bool hit = g[0] > 0.5f;
    hits += hit;
    bool ok = eid < 0 ? !hit : (hit && int(g[2] + 0.5f) == eid && std::fabs(g[1] - et) < 1e-3f);
    if (!ok && bad++ < 6)
      std::printf("  %s mismatch at (%.1f, %.1f): got hit %d t %.4f id %.0f prim %.0f, expected id %d t %.4f\n",
        label, x, y, int(hit), g[1], g[2], g[3], eid, et);
  }
  std::printf("%s: %u rays, %d hits, %d mismatches -> %s\n", label, N * N, hits, bad, bad ? "FAIL" : "PASS");
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  const char* spvPath = argc > 1 ? argv[1] : "nvr_rt_test.spv";
  if (argc > 2) SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", argv[2]);

  WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvrrttest";
  RegisterClassA(&wc);
  HWND win = CreateWindowExA(0, "nvrrttest", "nvrrttest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 48, nullptr, nullptr, wc.hInstance, nullptr);
  if (!win) { std::printf("window failed\n"); return 1; }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DADAPTER_IDENTIFIER9 ai = {}; d3d->GetAdapterIdentifier(0, 0, &ai);
  std::printf("adapter 0: %s\n", ai.Description);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = win;
  pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  IDirect3DDevice9* dev = nullptr;
  CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev), "CreateDevice");

  ID3D9NvrExt* ext = nullptr;
  CHECK(dev->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext), "QueryInterface(ID3D9NvrExt)");
  ID3D9NvrExtRt* rt = nullptr;
  CHECK(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&rt), "QueryInterface(ID3D9NvrExtRt)");
  NVR_EXT_RT_INFO info = {}; rt->GetRtInfo(&info);
  std::printf("rt: supported %d, max instances %u, max triangles %llu, reason '%s'\n", info.Supported, info.MaxInstances,
    (unsigned long long)info.MaxPrimitiveCount, info.Reason);
  if (!info.Supported) {
    NVR_EXT_RT_GEOMETRY_DESC gd = {};
    ID3D9NvrExtRtGeometry* g = nullptr;
    HRESULT hr = rt->CreateGeometry(&gd, &g);
    std::printf("unsupported: CreateGeometry -> 0x%08x (expected D3DERR_NOTAVAILABLE 0x%08x) -> %s\n", unsigned(hr), unsigned(D3DERR_NOTAVAILABLE),
      hr == D3DERR_NOTAVAILABLE ? "PASS" : "FAIL");
    return hr == D3DERR_NOTAVAILABLE ? 0 : 1;
  }

  // Ground: float3 + uv (stride 20), 16-bit list
  IDirect3DVertexBuffer9* vb1; IDirect3DIndexBuffer9* ib1;
  CHECK(dev->CreateVertexBuffer(4 * 20, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb1, nullptr), "vb1");
  { float* v; vb1->Lock(0, 0, (void**)&v, 0);
    const float q[4][5] = { { -10, -10, 0, 0, 0 }, { 10, -10, 0, 1, 0 }, { 10, 10, 0, 1, 1 }, { -10, 10, 0, 0, 1 } };
    std::memcpy(v, q, sizeof(q)); vb1->Unlock(); }
  CHECK(dev->CreateIndexBuffer(6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib1, nullptr), "ib1");
  { uint16_t* i; ib1->Lock(0, 0, (void**)&i, 0); const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 }; std::memcpy(i, q, sizeof(q)); ib1->Unlock(); }

  // Roof: float4 (stride 16), 2 padding vertices (BaseVertex 2), 32-bit strip starting at index 1 (index 0 is junk)
  IDirect3DVertexBuffer9* vb2; IDirect3DIndexBuffer9* ib2;
  CHECK(dev->CreateVertexBuffer(6 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb2, nullptr), "vb2");
  { float* v; vb2->Lock(0, 0, (void**)&v, 0);
    const float q[6][4] = { { 99, 99, 99, 1 }, { 99, 99, 99, 1 }, { -2, -2, 5, 1 }, { 2, -2, 5, 1 }, { 2, 2, 5, 1 }, { -2, 2, 5, 1 } };
    std::memcpy(v, q, sizeof(q)); vb2->Unlock(); }
  CHECK(dev->CreateIndexBuffer(5 * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib2, nullptr), "ib2");
  { uint32_t* i; ib2->Lock(0, 0, (void**)&i, 0); const uint32_t q[5] = { 777, 0, 1, 3, 2 }; std::memcpy(i, q, sizeof(q)); ib2->Unlock(); }

  // Post: half4 (stride 8), no index buffer, list of 2 triangles at z = 2 over [6, 7] x [6, 7]
  IDirect3DVertexBuffer9* vb3;
  CHECK(dev->CreateVertexBuffer(6 * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb3, nullptr), "vb3");
  { uint16_t* v; vb3->Lock(0, 0, (void**)&v, 0);
    const float q[6][3] = { { 6, 6, 2 }, { 7, 6, 2 }, { 7, 7, 2 }, { 6, 6, 2 }, { 7, 7, 2 }, { 6, 7, 2 } };
    for (int k = 0; k < 6; k++) { for (int c = 0; c < 3; c++) v[k * 4 + c] = toHalf(q[k][c]); v[k * 4 + 3] = toHalf(1.f); }
    vb3->Unlock(); }

  NVR_EXT_RT_RANGE r1 = { NVR_EXT_RT_TRIANGLE_LIST, 0, 2, 0 };
  NVR_EXT_RT_GEOMETRY_DESC g1 = { vb1, 0, 20, 4, NVR_EXT_RT_POSITION_FLOAT3, ib1, 1, &r1, 0, "ground" };
  NVR_EXT_RT_RANGE r2 = { NVR_EXT_RT_TRIANGLE_STRIP, 1, 2, 2 };
  NVR_EXT_RT_GEOMETRY_DESC g2 = { vb2, 0, 16, 6, NVR_EXT_RT_POSITION_FLOAT4, ib2, 1, &r2, 0, "roof" };
  NVR_EXT_RT_RANGE r3 = { NVR_EXT_RT_TRIANGLE_LIST, 0, 2, 0 };
  NVR_EXT_RT_GEOMETRY_DESC g3 = { vb3, 0, 8, 0, NVR_EXT_RT_POSITION_HALF4, nullptr, 1, &r3, 0, "post" };
  ID3D9NvrExtRtGeometry *ground, *roof, *post;
  CHECK(rt->CreateGeometry(&g1, &ground), "CreateGeometry ground");
  CHECK(rt->CreateGeometry(&g2, &roof), "CreateGeometry roof");
  CHECK(rt->CreateGeometry(&g3, &post), "CreateGeometry post");
  std::printf("geometries: %u + %u + %u triangles\n", ground->GetTriangleCount(), roof->GetTriangleCount(), post->GetTriangleCount());

  // A range past the end must be refused
  NVR_EXT_RT_RANGE bad = { NVR_EXT_RT_TRIANGLE_LIST, 3, 2, 0 };
  NVR_EXT_RT_GEOMETRY_DESC gb = g1; gb.pRanges = &bad; gb.pDebugName = "bad range";
  ID3D9NvrExtRtGeometry* gbad = nullptr;
  HRESULT hrBad = rt->CreateGeometry(&gb, &gbad);
  std::printf("out-of-range geometry -> 0x%08x -> %s\n", unsigned(hrBad), hrBad == D3DERR_INVALIDCALL ? "PASS" : "FAIL");
  int failures = hrBad == D3DERR_INVALIDCALL ? 0 : 1;

  ID3D9NvrExtRtScene* scene;
  CHECK(rt->CreateScene(64, "test scene", &scene), "CreateScene");

  std::vector<char> spv = readFile(spvPath);
  NVR_EXT_BINDING_DESC binds[] = { { 0, 0, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE, 0 } };
  NVR_EXT_COMPUTE_PIPELINE_DESC pd = {};
  pd.pSpirv = spv.data(); pd.SpirvSize = UINT(spv.size()); pd.BindingCount = 1; pd.pBindings = binds; pd.PushConstantSize = sizeof(Push);
  pd.pDebugName = "rt test";
  ID3D9NvrExtPipeline* pipe;
  CHECK(ext->CreateComputePipeline(&pd, &pipe), "CreateComputePipeline (ray query)");
  NVR_EXT_BUFFER_DESC bd = { N * N * 16, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC, "rt results" };
  ID3D9NvrExtBuffer* out; CHECK(ext->CreateBuffer(&bd, &out), "out buffer");

  // Empty scene first: every ray misses
  failures += checkPass(ext, dev, pipe, out, scene, {}, 20.f, -1.f, "empty scene, rays down");

  auto instance = [] (ID3D9NvrExtRtGeometry* g, float tx, uint32_t id) {
    NVR_EXT_RT_INSTANCE i = {}; i.pGeometry = g; i.InstanceId = id; i.Mask = 0xFF;
    i.Transform[0][0] = i.Transform[1][1] = i.Transform[2][2] = 1.f; i.Transform[0][3] = tx;
    return i;
  };
  std::vector<NVR_EXT_RT_INSTANCE> inst = { instance(ground, 0, 7), instance(roof, 1.f, 9), instance(nullptr, 0, 99), instance(post, 0, 11) };
  CHECK(rt->BuildScene(scene, UINT(inst.size()), inst.data()), "BuildScene 1");
  std::vector<Rect> rects = { { -10, -10, 10, 10, 0, 7, true }, { -1, -2, 3, 2, 5, 9, true }, { 6, 6, 7, 7, 2, 11, true } };
  failures += checkPass(ext, dev, pipe, out, scene, rects, 0.01f, 1.f, "scene 1, rays up");
  failures += checkPass(ext, dev, pipe, out, scene, rects, 20.f, -1.f, "scene 1, rays down");

  // The roof moved; the ground released by the caller (the scene keeps it until rebuilt)
  inst[1] = instance(roof, -1.f, 9);
  ground->Release(); ground = nullptr;
  failures += checkPass(ext, dev, pipe, out, scene, rects, 20.f, -1.f, "scene 1 after the caller released the ground");
  inst.erase(inst.begin());
  CHECK(rt->BuildScene(scene, UINT(inst.size()), inst.data()), "BuildScene 2");
  rects = { { -10, -10, 10, 10, 0, 7, false }, { -3, -2, 1, 2, 5, 9, true }, { 6, 6, 7, 7, 2, 11, true } };
  failures += checkPass(ext, dev, pipe, out, scene, rects, 20.f, -1.f, "scene 2 (roof moved, no ground), rays down");

  // Many frames of rebuilds (lifetime churn)
  for (int f = 0; f < 120; f++) {
    inst[0] = instance(roof, float(f % 3) - 1.f, 9);
    rt->BuildScene(scene, UINT(inst.size()), inst.data());
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0); dev->Present(nullptr, nullptr, nullptr, nullptr);
  }
  inst[0] = instance(roof, 1.f, 9);
  CHECK(rt->BuildScene(scene, UINT(inst.size()), inst.data()), "BuildScene 3");
  rects[1] = { -1, -2, 3, 2, 5, 9, true };
  failures += checkPass(ext, dev, pipe, out, scene, rects, 20.f, -1.f, "after 120 rebuilds");

  NVR_EXT_RT_STATS st = {}; rt->GetRtStats(&st);
  std::printf("stats: %u geometries, %llu triangles, %llu KiB structures, %llu built, %u scenes, last %u instances, %llu scene builds, %llu KiB scene\n",
    st.Geometries, (unsigned long long)st.Triangles, (unsigned long long)(st.GeometryBytes >> 10), (unsigned long long)st.GeometriesBuilt,
    st.Scenes, st.LastInstances, (unsigned long long)st.SceneBuilds, (unsigned long long)(st.SceneBytes >> 10));

  scene->Release(); roof->Release(); post->Release(); pipe->Release(); out->Release(); rt->Release(); ext->Release();
  vb1->Release(); ib1->Release(); vb2->Release(); ib2->Release(); vb3->Release();
  dev->Release(); d3d->Release();
  DestroyWindow(win);
  std::printf("%s (%d failures)\n", failures ? "RT TEST FAILED" : "RT TEST PASSED", failures);
  return failures ? 1 : 0;
}
